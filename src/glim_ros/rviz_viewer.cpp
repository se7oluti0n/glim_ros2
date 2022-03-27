#include <glim_ros/rviz_viewer.hpp>

#include <mutex>
#include <rclcpp/clock.hpp>

#define GLIM_ROS2
#include <gtsam_ext/types/frame_cpu.hpp>
#include <glim/frontend/callbacks.hpp>
#include <glim/backend/callbacks.hpp>
#include <glim/util/trajectory_manager.hpp>
#include <glim/util/ros_cloud_converter.hpp>

namespace glim {

RvizViewer::RvizViewer(rclcpp::Node& node) {
  tf_broadcaster = std::make_unique<tf2_ros::TransformBroadcaster>(node);

  points_pub = node.create_publisher<sensor_msgs::msg::PointCloud2>("/glim_ros/points", 10);
  map_pub = node.create_publisher<sensor_msgs::msg::PointCloud2>("/glim_ros/map", 1);

  odom_pub = node.create_publisher<nav_msgs::msg::Odometry>("/glim_ros/odom", 10);
  pose_pub = node.create_publisher<geometry_msgs::msg::PoseStamped>("/glim_ros/pose", 10);

  imu_frame_id = "imu";
  lidar_frame_id = "lidar";
  odom_frame_id = "odom";
  world_frame_id = "world";

  trajectory.reset(new TrajectoryManager);

  set_callbacks();

  kill_switch = false;
  thread = std::thread([this] {
    while (!kill_switch) {
      const auto expected = std::chrono::milliseconds(10);
      const auto t1 = std::chrono::high_resolution_clock::now();
      spin_once();
      const auto t2 = std::chrono::high_resolution_clock::now();

      if (t2 - t1 < expected) {
        std::this_thread::sleep_for(expected - (t2 - t1));
      }
    }
  });
}

RvizViewer::~RvizViewer() {
  kill_switch = true;
  thread.join();
}

void RvizViewer::set_callbacks() {
  using std::placeholders::_1;
  OdometryEstimationCallbacks::on_new_frame.add(std::bind(&RvizViewer::frontend_new_frame, this, _1));
  GlobalMappingCallbacks::on_update_submaps.add(std::bind(&RvizViewer::globalmap_on_update_submaps, this, _1));
}

void RvizViewer::frontend_new_frame(const EstimationFrame::ConstPtr& new_frame) {
  if (points_pub->get_subscription_count()) {
    std::string frame_id;
    switch (new_frame->frame_id) {
      case FrameID::LIDAR:
        frame_id = lidar_frame_id;
        break;
      case FrameID::IMU:
        frame_id = imu_frame_id;
        break;
      case FrameID::WORLD:
        frame_id = world_frame_id;
        break;
    }

    auto points = frame_to_pointcloud2(frame_id, new_frame->stamp, *new_frame->frame);
    points_pub->publish(*points);
  }

  const Eigen::Isometry3d T_odom_imu = new_frame->T_world_imu;
  const Eigen::Quaterniond quat_odom_imu(T_odom_imu.linear());

  const Eigen::Isometry3d T_lidar_imu = new_frame->T_lidar_imu;
  const Eigen::Quaterniond quat_lidar_imu(T_lidar_imu.linear());

  Eigen::Isometry3d T_world_odom;
  Eigen::Quaterniond quat_world_odom;

  Eigen::Isometry3d T_world_imu;
  Eigen::Quaterniond quat_world_imu;

  {
    std::lock_guard<std::mutex> lock(trajectory_mutex);
    trajectory->add_odom(new_frame->stamp, new_frame->T_world_imu);
    T_world_odom = trajectory->get_T_world_odom();
    quat_world_odom = Eigen::Quaterniond(T_world_odom.linear());

    T_world_imu = trajectory->odom2world(T_odom_imu);
    quat_world_imu = Eigen::Quaterniond(T_world_imu.linear());
  }

  // Odom -> IMU
  geometry_msgs::msg::TransformStamped trans;
  trans.header.stamp = rclcpp::Time(new_frame->stamp);
  trans.header.frame_id = odom_frame_id;
  trans.child_frame_id = imu_frame_id;
  trans.transform.translation.x = T_odom_imu.translation().x();
  trans.transform.translation.y = T_odom_imu.translation().y();
  trans.transform.translation.z = T_odom_imu.translation().z();
  trans.transform.rotation.x = quat_odom_imu.x();
  trans.transform.rotation.y = quat_odom_imu.y();
  trans.transform.rotation.z = quat_odom_imu.z();
  trans.transform.rotation.w = quat_odom_imu.w();
  tf_broadcaster->sendTransform(trans);

  // IMU -> LiDAR
  trans.header.frame_id = imu_frame_id;
  trans.child_frame_id = lidar_frame_id;
  trans.transform.translation.x = T_lidar_imu.translation().x();
  trans.transform.translation.y = T_lidar_imu.translation().y();
  trans.transform.translation.z = T_lidar_imu.translation().z();
  trans.transform.rotation.x = quat_lidar_imu.x();
  trans.transform.rotation.y = quat_lidar_imu.y();
  trans.transform.rotation.z = quat_lidar_imu.z();
  trans.transform.rotation.w = quat_lidar_imu.w();
  tf_broadcaster->sendTransform(trans);

  // World -> Odom
  trans.header.frame_id = world_frame_id;
  trans.child_frame_id = odom_frame_id;
  trans.transform.translation.x = T_world_odom.translation().x();
  trans.transform.translation.y = T_world_odom.translation().y();
  trans.transform.translation.z = T_world_odom.translation().z();
  trans.transform.rotation.x = quat_world_odom.x();
  trans.transform.rotation.y = quat_world_odom.y();
  trans.transform.rotation.z = quat_world_odom.z();
  trans.transform.rotation.w = quat_world_odom.w();
  tf_broadcaster->sendTransform(trans);

  if (odom_pub->get_subscription_count()) {
    nav_msgs::msg::Odometry odom;
    odom.header.stamp = rclcpp::Time(new_frame->stamp);
    odom.header.frame_id = odom_frame_id;
    odom.child_frame_id = imu_frame_id;
    odom.pose.pose.position.x = T_odom_imu.translation().x();
    odom.pose.pose.position.y = T_odom_imu.translation().y();
    odom.pose.pose.position.z = T_odom_imu.translation().z();
    odom.pose.pose.orientation.x = quat_odom_imu.x();
    odom.pose.pose.orientation.y = quat_odom_imu.y();
    odom.pose.pose.orientation.z = quat_odom_imu.z();
    odom.pose.pose.orientation.w = quat_odom_imu.w();
    odom_pub->publish(odom);
  }

  if (pose_pub->get_subscription_count()) {
    geometry_msgs::msg::PoseStamped pose;
    pose.header.stamp = rclcpp::Time(new_frame->stamp);
    pose.header.frame_id = world_frame_id;
    pose.pose.position.x = T_world_imu.translation().x();
    pose.pose.position.y = T_world_imu.translation().y();
    pose.pose.position.z = T_world_imu.translation().z();
    pose.pose.orientation.x = quat_world_imu.x();
    pose.pose.orientation.y = quat_world_imu.y();
    pose.pose.orientation.z = quat_world_imu.z();
    pose.pose.orientation.w = quat_world_imu.w();
    pose_pub->publish(pose);
  }
}

void RvizViewer::globalmap_on_update_submaps(const std::vector<SubMap::Ptr>& submaps) {
  const SubMap::ConstPtr latest_submap = submaps.back();

  const double stamp_endpoint_R = latest_submap->odom_frames.back()->stamp;
  const Eigen::Isometry3d T_world_endpoint_R = latest_submap->T_world_origin * latest_submap->T_origin_endpoint_R;
  {
    std::lock_guard<std::mutex> lock(trajectory_mutex);
    trajectory->update_anchor(stamp_endpoint_R, T_world_endpoint_R);
  }

  std::vector<Eigen::Isometry3d, Eigen::aligned_allocator<Eigen::Isometry3d>> submap_poses(submaps.size());
  for (int i = 0; i < submaps.size(); i++) {
    submap_poses[i] = submaps[i]->T_world_origin;
  }

  invoke([this, latest_submap, submap_poses] {
    this->submaps.push_back(latest_submap->frame);

    if (!map_pub->get_subscription_count()) {
      return;
    }

    int total_num_points = 0;
    for (const auto& submap : this->submaps) {
      total_num_points += submap->size();
    }

    gtsam_ext::FrameCPU::Ptr merged(new gtsam_ext::FrameCPU);
    merged->num_points = total_num_points;
    merged->points_storage.resize(total_num_points);
    merged->points = merged->points_storage.data();

    int begin = 0;
    for (int i = 0; i < this->submaps.size(); i++) {
      const auto& submap = this->submaps[i];
      std::transform(submap->points, submap->points + submap->size(), merged->points + begin, [&](const Eigen::Vector4d& p) { return submap_poses[i] * p; });
      begin += submap->size();
    }

    const rclcpp::Time now = rclcpp::Clock(rcl_clock_type_t::RCL_ROS_TIME).now();
    auto points_msg = frame_to_pointcloud2(world_frame_id, now.seconds(), *merged);
    map_pub->publish(*points_msg);
  });
}

void RvizViewer::invoke(const std::function<void()>& task) {
  std::lock_guard<std::mutex> lock(invoke_queue_mutex);
  invoke_queue.push_back(task);
}

void RvizViewer::spin_once() {
  std::lock_guard<std::mutex> lock(invoke_queue_mutex);
  for (const auto& task : invoke_queue) {
    task();
  }
  invoke_queue.clear();
}

}  // namespace glim