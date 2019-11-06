#include <ros/ros.h>

#include "cam_lidar_calibration/point_xyzir.h"
#include <pcl/point_cloud.h>
#include <pcl/common/intersections.h>

#include <pcl/filters/extract_indices.h>
#include <pcl/filters/impl/extract_indices.hpp>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/impl/passthrough.hpp>
#include <pcl/filters/project_inliers.h>
#include <pcl/filters/impl/project_inliers.hpp>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/impl/sac_segmentation.hpp>

#include <opencv2/calib3d.hpp>

#include <cv_bridge/cv_bridge.h>
#include <pcl_ros/point_cloud.h>
#include <sensor_msgs/image_encodings.h>

#include "cam_lidar_calibration/feature_extractor.h"

using cv::findChessboardCorners;
using cv::Mat_;
using cv::Size;
using cv::TermCriteria;

using PointCloud = pcl::PointCloud<pcl::PointXYZIR>;

int main(int argc, char** argv)
{
  // Initialize Node and handles
  ros::init(argc, argv, "FeatureExtractor");
  ros::NodeHandle n;

  cam_lidar_calibration::FeatureExtractor FeatureExtractor;
  FeatureExtractor.bypassInit();
  ros::spin();

  return 0;
}

namespace cam_lidar_calibration
{
void FeatureExtractor::onInit()
{
  // Creating ROS nodehandle
  ros::NodeHandle private_nh = ros::NodeHandle("~");
  ros::NodeHandle public_nh = ros::NodeHandle();
  ros::NodeHandle pnh = ros::NodeHandle("~");  // getMTPrivateNodeHandle();
  loadParams(public_nh, i_params);
  ROS_INFO("Input parameters loaded");

  it_.reset(new image_transport::ImageTransport(public_nh));
  it_p_.reset(new image_transport::ImageTransport(private_nh));

  // Dynamic reconfigure gui to set the experimental region bounds
  server = boost::make_shared<dynamic_reconfigure::Server<cam_lidar_calibration::boundsConfig>>(pnh);
  dynamic_reconfigure::Server<cam_lidar_calibration::boundsConfig>::CallbackType f;
  f = boost::bind(&FeatureExtractor::boundsCB, this, _1, _2);
  server->setCallback(f);

  // Synchronizer to get synchronized camera-lidar scan pairs
  image_sub_ = std::make_shared<image_sub_type>(public_nh, i_params.camera_topic, queue_rate_);
  pc_sub_ = std::make_shared<pc_sub_type>(public_nh, i_params.lidar_topic, queue_rate_);
  image_pc_sync_ = std::make_shared<message_filters::Synchronizer<ImageLidarSyncPolicy>>(
      ImageLidarSyncPolicy(queue_rate_), *image_sub_, *pc_sub_);
  image_pc_sync_->registerCallback(boost::bind(&FeatureExtractor::extractRegionOfInterest, this, _1, _2));

  roi_publisher = public_nh.advertise<cam_lidar_calibration::calibration_data>("roi/points", 10, true);
  pub_cloud = public_nh.advertise<PointCloud>("velodyne_features", 1);
  expt_region = public_nh.advertise<PointCloud>("Experimental_region", 10);
  sample_service_ = public_nh.advertiseService("sample", &FeatureExtractor::sampleCB, this);
  vis_pub = public_nh.advertise<visualization_msgs::Marker>("visualization_marker", 0);
  visPub = public_nh.advertise<visualization_msgs::Marker>("board_corners_3d", 0);
  image_publisher = it_->advertise("camera_features", 1);
  NODELET_INFO_STREAM("Camera Lidar Calibration");
}

bool FeatureExtractor::sampleCB(Sample::Request& req, Sample::Response& res)
{
  switch (req.operation)
  {
    case Sample::Request::CAPTURE:
      ROS_INFO("Capturing sample");
      break;
    case Sample::Request::DISCARD:
      ROS_INFO("Discarding last sample");
      break;
  }

  flag = req.operation;  // read flag published by rviz calibration panel
  return true;
}

void FeatureExtractor::boundsCB(cam_lidar_calibration::boundsConfig& config, uint32_t level)
{
  // Read the values corresponding to the motion of slider bars in reconfigure gui
  bounds_ = config;
  ROS_INFO("Reconfigure Request: %lf %lf %lf %lf %lf %lf", config.x_min, config.x_max, config.y_min, config.y_max,
           config.z_min, config.z_max);
}

void FeatureExtractor::passthrough(const PointCloud::ConstPtr& input_pc, PointCloud::Ptr& output_pc)
{
  PointCloud::Ptr x(new PointCloud);
  PointCloud::Ptr z(new PointCloud);
  // Filter out the experimental region
  pcl::PassThrough<pcl::PointXYZIR> pass;
  pass.setInputCloud(input_pc);
  pass.setFilterFieldName("x");
  pass.setFilterLimits(bounds_.x_min, bounds_.x_max);
  pass.filter(*x);
  pass.setInputCloud(x);
  pass.setFilterFieldName("z");
  pass.setFilterLimits(bounds_.z_min, bounds_.z_max);
  pass.filter(*z);
  pass.setInputCloud(z);
  pass.setFilterFieldName("y");
  pass.setFilterLimits(bounds_.y_min, bounds_.y_max);
  pass.filter(*output_pc);
}

auto FeatureExtractor::chessboardProjection(const std::vector<cv::Point2f>& corners,
                                            const cv_bridge::CvImagePtr& cv_ptr)
{
  // Now find the chessboard in 3D space
  cv::Point3f chessboard_centre(i_params.chessboard_pattern_size.width, i_params.chessboard_pattern_size.height, 0);
  chessboard_centre *= 0.5 * i_params.square_length;
  std::vector<cv::Point3f> corners_3d;
  for (int y = 0; y < i_params.chessboard_pattern_size.height; y++)
  {
    for (int x = 0; x < i_params.chessboard_pattern_size.width; x++)
    {
      corners_3d.push_back(cv::Point3f(x, y, 0) * i_params.square_length - chessboard_centre);
    }
  }

  // checkerboard corners, middle square corners, board corners and centre
  std::vector<cv::Point3f> board_corners_3d;
  // Board corner coordinates from the centre of the checkerboard
  for (auto x = 0; x < 2; x++)
  {
    for (auto y = 0; y < 2; y++)
    {
      board_corners_3d.push_back(
          cv::Point3f((-0.5 + x) * i_params.board_dimensions.width, (-0.5 + y) * i_params.board_dimensions.height, 0) -
          i_params.cb_translation_error);
    }
  }
  // Board centre coordinates from the centre of the checkerboard (due to incorrect placement of checkerbord on
  // board)
  board_corners_3d.push_back(-i_params.cb_translation_error);

  std::vector<cv::Point2f> corner_image_points, board_image_points;

  cv::Mat rvec(3, 3, cv::DataType<double>::type);  // Initialization for pinhole and fisheye cameras
  cv::Mat tvec(3, 1, cv::DataType<double>::type);

  if (i_params.fisheye_model)
  {
    // Undistort the image by applying the fisheye intrinsic parameters
    // the final input param is the camera matrix in the new or rectified coordinate frame.
    // We put this to be the same as i_params.cameramat or else it will be set to empty matrix by default.
    std::vector<cv::Point2f> corners_undistorted;
    cv::fisheye::undistortPoints(corners, corners_undistorted, i_params.cameramat, i_params.distcoeff,
                                 i_params.cameramat);
    cv::solvePnP(corners_3d, corners_undistorted, i_params.cameramat, cv::noArray(), rvec, tvec);
    cv::fisheye::projectPoints(corners_3d, corner_image_points, rvec, tvec, i_params.cameramat, i_params.distcoeff);
    cv::fisheye::projectPoints(board_corners_3d, board_image_points, rvec, tvec, i_params.cameramat,
                               i_params.distcoeff);
  }
  else
  {
    // Pinhole model
    cv::solvePnP(corners_3d, corners, i_params.cameramat, i_params.distcoeff, rvec, tvec);
    cv::projectPoints(corners_3d, rvec, tvec, i_params.cameramat, i_params.distcoeff, corner_image_points);
    cv::projectPoints(board_corners_3d, rvec, tvec, i_params.cameramat, i_params.distcoeff, board_image_points);
  }
  for (auto& point : corner_image_points)
  {
    cv::circle(cv_ptr->image, point, 5, CV_RGB(255, 0, 0), -1);
  }
  for (auto& point : board_image_points)
  {
    cv::circle(cv_ptr->image, point, 5, CV_RGB(255, 255, 0), -1);
  }

  // Return all the necessary coefficients
  return std::make_tuple(rvec, tvec, board_corners_3d);
}

std::optional<std::tuple<cv::Mat, cv::Mat>>
FeatureExtractor::locateChessboard(const sensor_msgs::Image::ConstPtr& image)
{
  // Convert to OpenCV image object
  cv_bridge::CvImagePtr cv_ptr;
  cv_ptr = cv_bridge::toCvCopy(image, sensor_msgs::image_encodings::BGR8);

  cv::Mat gray;
  cv::cvtColor(cv_ptr->image, gray, CV_BGR2GRAY);
  std::vector<cv::Point2f> corners;
  // Find checkerboard pattern in the image
  bool pattern_found = findChessboardCorners(gray, i_params.chessboard_pattern_size, corners,
                                             cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE);
  if (!pattern_found)
  {
    ROS_WARN("No chessboard found");
    return std::nullopt;
  }
  ROS_INFO("Chessboard found");
  // Find corner points with sub-pixel accuracy
  cornerSubPix(gray, corners, Size(11, 11), Size(-1, -1), TermCriteria(CV_TERMCRIT_EPS + CV_TERMCRIT_ITER, 30, 0.1));

  auto [rvec, tvec, board_corners_3d] = chessboardProjection(corners, cv_ptr);

  cv::Mat corner_vectors = cv::Mat::eye(3, 5, CV_64F);
  cv::Mat chessboard_normal = cv::Mat(1, 3, CV_64F);
  cv::Mat chessboardpose = cv::Mat::eye(4, 4, CV_64F);
  cv::Mat tmprmat = cv::Mat(3, 3, CV_64F);  // rotation matrix
  cv::Rodrigues(rvec, tmprmat);             // Euler angles to rotation matrix

  for (int j = 0; j < 3; j++)
  {
    for (int k = 0; k < 3; k++)
    {
      chessboardpose.at<double>(j, k) = tmprmat.at<double>(j, k);
    }
    chessboardpose.at<double>(j, 3) = tvec.at<double>(j);
  }

  chessboard_normal.at<double>(0) = 0;
  chessboard_normal.at<double>(1) = 0;
  chessboard_normal.at<double>(2) = 1;
  chessboard_normal = chessboard_normal * chessboardpose(cv::Rect(0, 0, 3, 3)).t();

  for (size_t k = 0; k < board_corners_3d.size(); k++)
  {
    // take every point in board_corners_3d set
    cv::Point3f pt(board_corners_3d[k]);
    for (int i = 0; i < 3; i++)
    {
      // Transform it to obtain the coordinates in cam frame
      corner_vectors.at<double>(i, k) = chessboardpose.at<double>(i, 0) * pt.x +
                                        chessboardpose.at<double>(i, 1) * pt.y + chessboardpose.at<double>(i, 3);
    }
  }
  // Publish the image with all the features marked in it
  ROS_INFO("Publishing chessboard image");
  image_publisher.publish(cv_ptr->toImageMsg());
  return std::make_optional(std::make_tuple(corner_vectors, chessboard_normal));
}

// Extract features of interest
void FeatureExtractor::extractRegionOfInterest(const sensor_msgs::Image::ConstPtr& image,
                                               const PointCloud::ConstPtr& pointcloud)
{
  PointCloud::Ptr cloud_bounded(new PointCloud);
  passthrough(pointcloud, cloud_bounded);

  // Publish the experimental region point cloud
  expt_region.publish(cloud_bounded);

  if (flag == Sample::Request::CAPTURE)
  {
    flag = 0;  // Reset the capture flag

    ROS_INFO("Processing sample");

    auto retval = locateChessboard(image);
    if (!retval)
    {
      return;
    }
    auto [corner_vectors, chessboard_normal] = *retval;
    //////////////// POINT CLOUD FEATURES //////////////////

    PointCloud::Ptr cloud_filtered(new PointCloud), corrected_plane(new PointCloud);
    sensor_msgs::PointCloud2 cloud_final;
    // Filter out the board point cloud
    // find the point with max height(z val) in cloud_passthrough
    double z_max = cloud_bounded->points[0].z;
    for (size_t i = 0; i < cloud_bounded->points.size(); ++i)
    {
      if (cloud_bounded->points[i].z > z_max)
      {
        z_max = cloud_bounded->points[i].z;
      }
    }
    // subtract by approximate diagonal length (in metres)
    double z_min = z_max - diagonal;
    pcl::PassThrough<pcl::PointXYZIR> pass_z;
    pass_z.setFilterFieldName("z");
    pass_z.setFilterLimits(z_min, z_max);
    pass_z.filter(*cloud_filtered);  // board point cloud

    // Fit a plane through the board point cloud
    pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients());
    pcl::PointIndices::Ptr inliers(new pcl::PointIndices());
    pcl::SACSegmentation<pcl::PointXYZIR> seg;
    seg.setOptimizeCoefficients(true);
    seg.setModelType(pcl::SACMODEL_PLANE);
    seg.setMethodType(pcl::SAC_RANSAC);
    seg.setMaxIterations(1000);
    seg.setDistanceThreshold(0.004);
    pcl::ExtractIndices<pcl::PointXYZIR> extract;
    seg.setInputCloud(cloud_filtered);
    seg.segment(*inliers, *coefficients);
    // Check that segmentation succeeded
    if (coefficients->values.size() < 3)
    {
      ROS_WARN("Checkerboard plane segmentation failed");
      return;
    }
    // Plane normal vector magnitude
    float mag =
        sqrt(pow(coefficients->values[0], 2) + pow(coefficients->values[1], 2) + pow(coefficients->values[2], 2));

    // Project the inliers on the fit plane
    PointCloud::Ptr cloud_projected(new PointCloud);
    pcl::ProjectInliers<pcl::PointXYZIR> proj;
    proj.setModelType(pcl::SACMODEL_PLANE);
    proj.setInputCloud(cloud_filtered);
    proj.setModelCoefficients(coefficients);
    proj.filter(*cloud_projected);

    // Publish the projected inliers
    pcl::toROSMsg(*cloud_filtered, cloud_final);
    pub_cloud.publish(cloud_final);

    // FIND THE MAX AND MIN POINTS IN EVERY RING CORRESPONDING TO THE BOARD

    // First: Sort out the points in the point cloud according to their ring numbers
    std::vector<std::deque<pcl::PointXYZIR*>> candidate_segments(i_params.lidar_ring_count);

    double x_projected = 0;
    double y_projected = 0;
    double z_projected = 0;
    for (size_t i = 0; i < cloud_projected->points.size(); ++i)
    {
      x_projected += cloud_projected->points[i].x;
      y_projected += cloud_projected->points[i].y;
      z_projected += cloud_projected->points[i].z;

      int ring_number = static_cast<int>(cloud_projected->points[i].ring);

      // push back the points in a particular ring number
      candidate_segments[ring_number].push_back(&(cloud_projected->points[i]));
    }

    // Second: Arrange points in every ring in descending order of y coordinate
    pcl::PointXYZIR max, min;
    PointCloud::Ptr max_points(new PointCloud);
    PointCloud::Ptr min_points(new PointCloud);
    for (int i = 0; static_cast<size_t>(i) < candidate_segments.size(); i++)
    {
      if (candidate_segments[i].size() == 0)  // If no points belong to a aprticular ring number
      {
        continue;
      }
      for (size_t j = 0; j < candidate_segments[i].size(); j++)
      {
        for (size_t k = j + 1; k < candidate_segments[i].size(); k++)
        {
          // If there is a larger element found on right of the point, swap
          if (candidate_segments[i][j]->y < candidate_segments[i][k]->y)
          {
            pcl::PointXYZIR temp;
            temp = *candidate_segments[i][k];
            *candidate_segments[i][k] = *candidate_segments[i][j];
            *candidate_segments[i][j] = temp;
          }
        }
      }
    }

    // Third: Find minimum and maximum points in a ring
    for (int i = 0; static_cast<size_t>(i) < candidate_segments.size(); i++)
    {
      if (candidate_segments[i].size() == 0)
      {
        continue;
      }
      max = *candidate_segments[i][0];
      min = *candidate_segments[i][candidate_segments[i].size() - 1];
      min_points->push_back(min);
      max_points->push_back(max);
    }

    // Fit lines through minimum and maximum points
    pcl::ModelCoefficients::Ptr coefficients_left_up(new pcl::ModelCoefficients);
    pcl::PointIndices::Ptr inliers_left_up(new pcl::PointIndices);
    pcl::ModelCoefficients::Ptr coefficients_left_dwn(new pcl::ModelCoefficients);
    pcl::PointIndices::Ptr inliers_left_dwn(new pcl::PointIndices);
    pcl::ModelCoefficients::Ptr coefficients_right_up(new pcl::ModelCoefficients);
    pcl::PointIndices::Ptr inliers_right_up(new pcl::PointIndices);
    pcl::ModelCoefficients::Ptr coefficients_right_dwn(new pcl::ModelCoefficients);
    pcl::PointIndices::Ptr inliers_right_dwn(new pcl::PointIndices);
    PointCloud::Ptr cloud_f(new PointCloud), cloud_f1(new PointCloud);

    seg.setModelType(pcl::SACMODEL_LINE);
    seg.setMethodType(pcl::SAC_RANSAC);
    seg.setDistanceThreshold(0.02);
    seg.setInputCloud(max_points);
    seg.segment(*inliers_left_up, *coefficients_left_up);  // Fitting line1 through max points
    extract.setInputCloud(max_points);
    extract.setIndices(inliers_left_up);
    extract.setNegative(true);
    extract.filter(*cloud_f);
    seg.setInputCloud(cloud_f);
    seg.segment(*inliers_left_dwn, *coefficients_left_dwn);  // Fitting line2 through max points
    seg.setInputCloud(min_points);
    seg.segment(*inliers_right_up, *coefficients_right_up);  // Fitting line1 through min points
    extract.setInputCloud(min_points);
    extract.setIndices(inliers_right_up);
    extract.setNegative(true);
    extract.filter(*cloud_f1);
    seg.setInputCloud(cloud_f1);
    seg.segment(*inliers_right_dwn, *coefficients_right_dwn);  // Fitting line2 through min points

    // Find out 2 (out of the four) intersection points
    Eigen::Vector4f Point_l;
    pcl::PointCloud<pcl::PointXYZ>::Ptr basic_cloud_ptr(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointXYZ basic_point;  // intersection points stored here
    if (pcl::lineWithLineIntersection(*coefficients_left_up, *coefficients_left_dwn, Point_l))
    {
      basic_point.x = Point_l[0];
      basic_point.y = Point_l[1];
      basic_point.z = Point_l[2];
      basic_cloud_ptr->points.push_back(basic_point);
    }
    if (pcl::lineWithLineIntersection(*coefficients_right_up, *coefficients_right_dwn, Point_l))
    {
      basic_point.x = Point_l[0];
      basic_point.y = Point_l[1];
      basic_point.z = Point_l[2];
      basic_cloud_ptr->points.push_back(basic_point);
    }

    // To determine the other 2 intersection points

    // Find out the diagonal vector(joining the line intersections from min_points and max_points)
    Eigen::Vector3f diagonal(basic_cloud_ptr->points[1].x - basic_cloud_ptr->points[0].x,
                             basic_cloud_ptr->points[1].y - basic_cloud_ptr->points[0].y,
                             basic_cloud_ptr->points[1].z - basic_cloud_ptr->points[0].z);
    // Take the first points in min_points and max_points,
    // find the vectors joining them and the (intersection point of the two lines in min_points)
    Eigen::Vector3f p1(min_points->points[inliers_right_dwn->indices[0]].x - basic_cloud_ptr->points[0].x,
                       min_points->points[inliers_right_dwn->indices[0]].y - basic_cloud_ptr->points[0].y,
                       min_points->points[inliers_right_dwn->indices[0]].z - basic_cloud_ptr->points[0].z);
    Eigen::Vector3f p2(max_points->points[inliers_left_dwn->indices[0]].x - basic_cloud_ptr->points[0].x,
                       max_points->points[inliers_left_dwn->indices[0]].y - basic_cloud_ptr->points[0].y,
                       max_points->points[inliers_left_dwn->indices[0]].z - basic_cloud_ptr->points[0].z);
    // To check if p1 and p2 lie on the same side or opp. side of diagonal vector.
    // If they lie on the same side
    if ((diagonal.cross(p1)).dot(diagonal.cross(p2)) > 0)
    {
      // Find out the line intersection between particular lines in min_points and max_points
      if (pcl::lineWithLineIntersection(*coefficients_left_dwn, *coefficients_right_up, Point_l))
      {
        basic_point.x = Point_l[0];
        basic_point.y = Point_l[1];
        basic_point.z = Point_l[2];
        basic_cloud_ptr->points.push_back(basic_point);
      }
      if (pcl::lineWithLineIntersection(*coefficients_left_up, *coefficients_right_dwn, Point_l))
      {
        basic_point.x = Point_l[0];
        basic_point.y = Point_l[1];
        basic_point.z = Point_l[2];
        basic_cloud_ptr->points.push_back(basic_point);
      }
    }
    // Else if they lie on the opp. side
    else
    {
      // Find out the line intersection between other lines in min_points and max_points
      if (pcl::lineWithLineIntersection(*coefficients_left_dwn, *coefficients_right_dwn, Point_l))
      {
        basic_point.x = Point_l[0];
        basic_point.y = Point_l[1];
        basic_point.z = Point_l[2];
        basic_cloud_ptr->points.push_back(basic_point);
      }
      if (pcl::lineWithLineIntersection(*coefficients_left_up, *coefficients_right_up, Point_l))
      {
        basic_point.x = Point_l[0];
        basic_point.y = Point_l[1];
        basic_point.z = Point_l[2];
        basic_cloud_ptr->points.push_back(basic_point);
      }
    }

    // input data
    sample_data.velodynepoint[0] = (basic_cloud_ptr->points[0].x + basic_cloud_ptr->points[1].x) * 1000 / 2;
    sample_data.velodynepoint[1] = (basic_cloud_ptr->points[0].y + basic_cloud_ptr->points[1].y) * 1000 / 2;
    sample_data.velodynepoint[2] = (basic_cloud_ptr->points[0].z + basic_cloud_ptr->points[1].z) * 1000 / 2;
    sample_data.velodynenormal[0] = -coefficients->values[0] / mag;
    sample_data.velodynenormal[1] = -coefficients->values[1] / mag;
    sample_data.velodynenormal[2] = -coefficients->values[2] / mag;
    double top_down_radius =
        sqrt(pow(sample_data.velodynepoint[0] / 1000, 2) + pow(sample_data.velodynepoint[1] / 1000, 2));
    double x_comp = sample_data.velodynepoint[0] / 1000 + sample_data.velodynenormal[0] / 2;
    double y_comp = sample_data.velodynepoint[1] / 1000 + sample_data.velodynenormal[1] / 2;
    double vector_dist = sqrt(pow(x_comp, 2) + pow(y_comp, 2));
    if (vector_dist > top_down_radius)
    {
      sample_data.velodynenormal[0] = -sample_data.velodynenormal[0];
      sample_data.velodynenormal[1] = -sample_data.velodynenormal[1];
      sample_data.velodynenormal[2] = -sample_data.velodynenormal[2];
    }
    sample_data.camerapoint[0] = corner_vectors.at<double>(0, 4);
    sample_data.camerapoint[1] = corner_vectors.at<double>(1, 4);
    sample_data.camerapoint[2] = corner_vectors.at<double>(2, 4);
    sample_data.cameranormal[0] = chessboard_normal.at<double>(0);
    sample_data.cameranormal[1] = chessboard_normal.at<double>(1);
    sample_data.cameranormal[2] = chessboard_normal.at<double>(2);
    sample_data.velodynecorner[0] = basic_cloud_ptr->points[2].x;
    sample_data.velodynecorner[1] = basic_cloud_ptr->points[2].y;
    sample_data.velodynecorner[2] = basic_cloud_ptr->points[2].z;

    // Visualize 4 corner points of velodyne board, the board edge lines and the centre point
    visualization_msgs::Marker marker1, line_strip, corners_board;
    marker1.header.frame_id = line_strip.header.frame_id = corners_board.header.frame_id = "/velodyne_front_link";
    marker1.header.stamp = line_strip.header.stamp = corners_board.header.stamp = ros::Time();
    marker1.ns = line_strip.ns = corners_board.ns = "my_sphere";
    line_strip.id = 10;
    marker1.id = 11;
    marker1.type = visualization_msgs::Marker::POINTS;
    line_strip.type = visualization_msgs::Marker::LINE_STRIP;
    corners_board.type = visualization_msgs::Marker::SPHERE;
    marker1.action = line_strip.action = corners_board.action = visualization_msgs::Marker::ADD;
    marker1.pose.orientation.w = line_strip.pose.orientation.w = corners_board.pose.orientation.w = 1.0;
    marker1.scale.x = 0.02;
    marker1.scale.y = 0.02;
    corners_board.scale.x = 0.04;
    corners_board.scale.y = 0.04;
    corners_board.scale.z = 0.04;
    line_strip.scale.x = 0.009;
    marker1.color.a = line_strip.color.a = corners_board.color.a = 1.0;
    line_strip.color.b = 1.0;
    marker1.color.b = marker1.color.g = marker1.color.r = 1.0;

    for (int i = 0; i < 5; i++)
    {
      if (i < 4)
      {
        corners_board.pose.position.x = basic_cloud_ptr->points[i].x;
        corners_board.pose.position.y = basic_cloud_ptr->points[i].y;
        corners_board.pose.position.z = basic_cloud_ptr->points[i].z;
      }
      else
      {
        corners_board.pose.position.x = sample_data.velodynepoint[0] / 1000;
        corners_board.pose.position.y = sample_data.velodynepoint[1] / 1000;
        corners_board.pose.position.z = sample_data.velodynepoint[2] / 1000;
      }

      corners_board.id = i;
      if (corners_board.id == 0)
        corners_board.color.b = 1.0;
      else if (corners_board.id == 1)
      {
        corners_board.color.b = 0.0;
        corners_board.color.g = 1.0;
      }
      else if (corners_board.id == 2)
      {
        corners_board.color.b = 0.0;
        corners_board.color.g = 0.0;
        corners_board.color.r = 1.0;
      }
      else if (corners_board.id == 3)
      {
        corners_board.color.b = 0.0;
        corners_board.color.r = 1.0;
        corners_board.color.g = 1.0;
      }
      else if (corners_board.id == 4)
      {
        corners_board.color.b = 1.0;
        corners_board.color.r = 1.0;
        corners_board.color.g = 1.0;
      }
      visPub.publish(corners_board);
    }

    // Visualize minimum and maximum points
    visualization_msgs::Marker minmax;
    minmax.header.frame_id = "/velodyne_front_link";
    minmax.header.stamp = ros::Time();
    minmax.ns = "my_sphere";
    minmax.type = visualization_msgs::Marker::SPHERE;
    minmax.action = visualization_msgs::Marker::ADD;
    minmax.pose.orientation.w = 1.0;
    minmax.scale.x = 0.02;
    minmax.scale.y = 0.02;
    minmax.scale.z = 0.02;
    minmax.color.a = 1.0;  // Don't forget to set the alpha!
    size_t y_min_pts = 0;
    for (y_min_pts = 0; y_min_pts < min_points->points.size(); y_min_pts++)
    {
      minmax.id = y_min_pts + 13;
      minmax.pose.position.x = min_points->points[y_min_pts].x;
      minmax.pose.position.y = min_points->points[y_min_pts].y;
      minmax.pose.position.z = min_points->points[y_min_pts].z;
      minmax.color.b = 1.0;
      minmax.color.r = 1.0;
      minmax.color.g = 0.0;
      visPub.publish(minmax);
    }
    for (size_t y_max_pts = 0; y_max_pts < max_points->points.size(); y_max_pts++)
    {
      minmax.id = y_min_pts + 13 + y_max_pts;
      minmax.pose.position.x = max_points->points[y_max_pts].x;
      minmax.pose.position.y = max_points->points[y_max_pts].y;
      minmax.pose.position.z = max_points->points[y_max_pts].z;
      minmax.color.r = 0.0;
      minmax.color.g = 1.0;
      minmax.color.b = 1.0;
      visPub.publish(minmax);
    }
    // Draw board edge lines
    for (int i = 0; i < 2; i++)
    {
      geometry_msgs::Point p;
      p.x = basic_cloud_ptr->points[1 - i].x;
      p.y = basic_cloud_ptr->points[1 - i].y;
      p.z = basic_cloud_ptr->points[1 - i].z;
      marker1.points.push_back(p);
      line_strip.points.push_back(p);
      p.x = basic_cloud_ptr->points[3 - i].x;
      p.y = basic_cloud_ptr->points[3 - i].y;
      p.z = basic_cloud_ptr->points[3 - i].z;
      marker1.points.push_back(p);
      line_strip.points.push_back(p);
    }

    geometry_msgs::Point p;
    p.x = basic_cloud_ptr->points[1].x;
    p.y = basic_cloud_ptr->points[1].y;
    p.z = basic_cloud_ptr->points[1].z;
    marker1.points.push_back(p);
    line_strip.points.push_back(p);
    p.x = basic_cloud_ptr->points[0].x;
    p.y = basic_cloud_ptr->points[0].y;
    p.z = basic_cloud_ptr->points[0].z;
    marker1.points.push_back(p);
    line_strip.points.push_back(p);

    // Publish board edges
    visPub.publish(line_strip);

    // Visualize board normal vector
    marker.header.frame_id = "/velodyne_front_link";
    marker.header.stamp = ros::Time();
    marker.ns = "my_namespace";
    marker.id = 12;
    marker.type = visualization_msgs::Marker::ARROW;
    marker.action = visualization_msgs::Marker::ADD;
    marker.scale.x = 0.02;
    marker.scale.y = 0.04;
    marker.scale.z = 0.06;
    marker.color.a = 1.0;
    marker.color.r = 0.0;
    marker.color.g = 0.0;
    marker.color.b = 1.0;
    geometry_msgs::Point start, end;
    start.x = sample_data.velodynepoint[0] / 1000;
    start.y = sample_data.velodynepoint[1] / 1000;
    start.z = sample_data.velodynepoint[2] / 1000;
    end.x = start.x + sample_data.velodynenormal[0] / 2;
    end.y = start.y + sample_data.velodynenormal[1] / 2;
    end.z = start.z + sample_data.velodynenormal[2] / 2;
    marker.points.resize(2);
    marker.points[0].x = start.x;
    marker.points[0].y = start.y;
    marker.points[0].z = start.z;
    marker.points[1].x = end.x;
    marker.points[1].y = end.y;
    marker.points[1].z = end.z;
    // Publish Board normal
    vis_pub.publish(marker);

    // Feature data is published(chosen) only if 'enter' is pressed
    roi_publisher.publish(sample_data);
  }  // if (flag == Sample::Request::CAPTURE)
}  // End of extractRegionOfInterest

}  // namespace cam_lidar_calibration
