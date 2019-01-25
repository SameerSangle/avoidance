#include "planner_functions.h"

#include <numeric>

namespace avoidance {

// trim the point cloud so that only points inside the bounding box are
// considered
void filterPointCloud(
    pcl::PointCloud<pcl::PointXYZ> &cropped_cloud,
    Eigen::Vector3f &closest_point, double &distance_to_closest_point,
    int &counter_backoff,
    const std::vector<pcl::PointCloud<pcl::PointXYZ>> &complete_cloud,
    double min_cloud_size, double min_dist_backoff, Box histogram_box,
    const Eigen::Vector3f &position, double min_realsense_dist) {
  cropped_cloud.points.clear();
  cropped_cloud.width = 0;
  distance_to_closest_point = HUGE_VAL;
  float distance;
  counter_backoff = 0;

  for (size_t i = 0; i < complete_cloud.size(); ++i) {
    for (const pcl::PointXYZ &xyz : complete_cloud[i]) {
      // Check if the point is invalid
      if (!std::isnan(xyz.x) && !std::isnan(xyz.y) && !std::isnan(xyz.z)) {
        if (histogram_box.isPointWithinBox(xyz.x, xyz.y, xyz.z)) {
          distance = (position - toEigen(xyz)).norm();
          if (distance > min_realsense_dist &&
              distance < histogram_box.radius_) {
            cropped_cloud.points.push_back(pcl::PointXYZ(xyz.x, xyz.y, xyz.z));
            if (distance < distance_to_closest_point) {
              distance_to_closest_point = distance;
              closest_point = toEigen(xyz);
            }
            if (distance < min_dist_backoff) {
              counter_backoff++;
            }
          }
        }
      }
    }
  }

  cropped_cloud.header.stamp = complete_cloud[0].header.stamp;
  cropped_cloud.header.frame_id = complete_cloud[0].header.frame_id;
  cropped_cloud.height = 1;
  cropped_cloud.width = cropped_cloud.points.size();
  if (cropped_cloud.points.size() <= min_cloud_size) {
    cropped_cloud.points.clear();
    cropped_cloud.width = 0;
  }
}

// Calculate FOV. Azimuth angle is wrapped, elevation is not!
void calculateFOV(double h_fov, double v_fov, std::vector<int> &z_FOV_idx,
                  int &e_FOV_min, int &e_FOV_max, double yaw, double pitch) {
  double z_FOV_max =
      std::round((-yaw * 180.0 / M_PI + h_fov / 2.0 + 270.0) / ALPHA_RES) - 1;
  double z_FOV_min =
      std::round((-yaw * 180.0 / M_PI - h_fov / 2.0 + 270.0) / ALPHA_RES) - 1;
  e_FOV_max =
      std::round((-pitch * 180.0 / M_PI + v_fov / 2.0 + 90.0) / ALPHA_RES) - 1;
  e_FOV_min =
      std::round((-pitch * 180.0 / M_PI - v_fov / 2.0 + 90.0) / ALPHA_RES) - 1;

  if (z_FOV_max >= GRID_LENGTH_Z && z_FOV_min >= GRID_LENGTH_Z) {
    z_FOV_max -= GRID_LENGTH_Z;
    z_FOV_min -= GRID_LENGTH_Z;
  }
  if (z_FOV_max < 0 && z_FOV_min < 0) {
    z_FOV_max += GRID_LENGTH_Z;
    z_FOV_min += GRID_LENGTH_Z;
  }

  z_FOV_idx.clear();
  if (z_FOV_max >= GRID_LENGTH_Z && z_FOV_min < GRID_LENGTH_Z) {
    for (int i = 0; i < z_FOV_max - GRID_LENGTH_Z; i++) {
      z_FOV_idx.push_back(i);
    }
    for (int i = z_FOV_min; i < GRID_LENGTH_Z; i++) {
      z_FOV_idx.push_back(i);
    }
  } else if (z_FOV_min < 0 && z_FOV_max >= 0) {
    for (int i = 0; i < z_FOV_max; i++) {
      z_FOV_idx.push_back(i);
    }
    for (int i = z_FOV_min + GRID_LENGTH_Z; i < GRID_LENGTH_Z; i++) {
      z_FOV_idx.push_back(i);
    }
  } else {
    for (int i = z_FOV_min; i < z_FOV_max; i++) {
      z_FOV_idx.push_back(i);
    }
  }
}

// Build histogram estimate from reprojected points
void propagateHistogram(Histogram &polar_histogram_est,
                        pcl::PointCloud<pcl::PointXYZ> reprojected_points,
                        std::vector<double> reprojected_points_age,
                        std::vector<double> reprojected_points_dist,
                        geometry_msgs::PoseStamped position) {
  Eigen::MatrixXi counter(GRID_LENGTH_E / 2, GRID_LENGTH_Z / 2);
  counter.fill(0);
  for (size_t i = 0; i < reprojected_points.points.size(); i++) {
    float e_angle = elevationAnglefromCartesian(
        toEigen(reprojected_points.points[i]), toEigen(position.pose.position));
    float z_angle = azimuthAnglefromCartesian(
        toEigen(reprojected_points.points[i]), toEigen(position.pose.position));

    int e_ind = elevationAngletoIndex(e_angle, 2 * ALPHA_RES);
    int z_ind = azimuthAngletoIndex(z_angle, 2 * ALPHA_RES);

    counter(e_ind, z_ind) += 1;
    polar_histogram_est.set_age(e_ind, z_ind,
                                polar_histogram_est.get_age(e_ind, z_ind) +
                                    0.25 * reprojected_points_age[i]);
    polar_histogram_est.set_dist(e_ind, z_ind,
                                 polar_histogram_est.get_dist(e_ind, z_ind) +
                                     0.25 * reprojected_points_dist[i]);
  }

  for (int e = 0; e < GRID_LENGTH_E / 2; e++) {
    for (int z = 0; z < GRID_LENGTH_Z / 2; z++) {
      if (counter(e, z) >= 6) {
        polar_histogram_est.set_dist(
            e, z, polar_histogram_est.get_dist(e, z) / counter(e, z));
        polar_histogram_est.set_age(
            e, z, polar_histogram_est.get_age(e, z) / counter(e, z));
      } else {  // not enough points to confidently block cell
        polar_histogram_est.set_dist(e, z, 0);
        polar_histogram_est.set_age(e, z, 0);
      }
    }
  }

  // Upsample propagated histogram
  polar_histogram_est.upsample();
}

// Generate new histogram from pointcloud
void generateNewHistogram(Histogram &polar_histogram,
                          const pcl::PointCloud<pcl::PointXYZ> &cropped_cloud,
                          geometry_msgs::PoseStamped position) {
  Eigen::MatrixXi counter(GRID_LENGTH_E, GRID_LENGTH_Z);
  counter.fill(0);
  for (auto xyz : cropped_cloud) {
    Eigen::Vector3f p = toEigen(xyz);
    float dist = (p - toEigen(position.pose.position)).norm();
    int e_angle =
        elevationAnglefromCartesian(p, toEigen(position.pose.position));
    int z_angle = azimuthAnglefromCartesian(p, toEigen(position.pose.position));

    int e_ind = elevationAngletoIndex(e_angle, ALPHA_RES);
    int z_ind = azimuthAngletoIndex(z_angle, ALPHA_RES);

    counter(e_ind, z_ind) += 1;
    polar_histogram.set_dist(e_ind, z_ind,
                             polar_histogram.get_dist(e_ind, z_ind) + dist);
  }

  // Normalize and get mean in distance bins
  for (int e = 0; e < GRID_LENGTH_E; e++) {
    for (int z = 0; z < GRID_LENGTH_Z; z++) {
      if (counter(e, z) > 0) {
        polar_histogram.set_dist(
            e, z, polar_histogram.get_dist(e, z) / counter(e, z));
      }
    }
  }
}

// Combine propagated histogram and new histogram to the final binary histogram
void combinedHistogram(bool &hist_empty, Histogram &new_hist,
                       Histogram propagated_hist, bool waypoint_outside_FOV,
                       std::vector<int> z_FOV_idx, int e_FOV_min,
                       int e_FOV_max) {
  hist_empty = true;
  for (int e = 0; e < GRID_LENGTH_E; e++) {
    for (int z = 0; z < GRID_LENGTH_Z; z++) {
      if (std::find(z_FOV_idx.begin(), z_FOV_idx.end(), z) != z_FOV_idx.end() &&
          e > e_FOV_min && e < e_FOV_max) {  // inside FOV
        if (new_hist.get_dist(e, z) > 0) {
          new_hist.set_age(e, z, 1);
          hist_empty = false;
        }
      } else {
        if (propagated_hist.get_dist(e, z) > 0) {
          if (waypoint_outside_FOV) {
            new_hist.set_age(e, z, propagated_hist.get_age(e, z));
          } else {
            new_hist.set_age(e, z, propagated_hist.get_age(e, z) + 1);
          }
          hist_empty = false;
        }
        if (new_hist.get_dist(e, z) > 0) {
          new_hist.set_age(e, z, 1);
          hist_empty = false;
        }
        if (propagated_hist.get_dist(e, z) > 0 &&
            new_hist.get_dist(e, z) < 0.001) {
          new_hist.set_dist(e, z, propagated_hist.get_dist(e, z));
        }
      }
    }
  }
}

void compressHistogramElevation(Histogram &new_hist, Histogram input_hist) {
  int vertical_FOV_range_sensor = 20;
  int lower_index = elevationAngletoIndex(
      -(float)(vertical_FOV_range_sensor / 2.0), ALPHA_RES);
  int upper_index = elevationAngletoIndex(
      (float)(vertical_FOV_range_sensor / 2.0), ALPHA_RES);

  for (int e = lower_index; e <= upper_index; e++) {
    for (int z = 0; z < GRID_LENGTH_Z; z++) {
      if (input_hist.get_dist(e, z) > 0) {
        if (input_hist.get_dist(e, z) < new_hist.get_dist(0, z) ||
            (new_hist.get_dist(0, z) == 0.0))
          new_hist.set_dist(0, z, input_hist.get_dist(e, z));
      }
    }
  }
}

void getCostMatrix(const Histogram &histogram, const Eigen::Vector3f &goal,
                   const Eigen::Vector3f &position,
                   const Eigen::Vector3f &last_sent_waypoint,
                   costParameters cost_params, bool only_yawed,
                   Eigen::MatrixXd &cost_matrix) {
  // reset cost matrix to zero
  cost_matrix.resize(GRID_LENGTH_E, GRID_LENGTH_Z);
  cost_matrix.fill(0.0);

  // fill in cost matrix
  for (int e_index = 0; e_index < GRID_LENGTH_E; e_index++) {
    for (int z_index = 0; z_index < GRID_LENGTH_Z; z_index++) {
      double e_angle = elevationIndexToAngle(e_index, ALPHA_RES);
      double z_angle = azimuthIndexToAngle(z_index, ALPHA_RES);
      double obstacle_distance = histogram.get_dist(e_index, z_index);
      cost_matrix(e_index, z_index) =
          costFunction(e_angle, z_angle, obstacle_distance, goal, position,
                       last_sent_waypoint, cost_params, only_yawed);
    }
  }

  unsigned int smooth_radius = 3;
  smoothPolarMatrix(cost_matrix, smooth_radius);
  smooth_radius = 2;
  smoothPolarMatrix(cost_matrix, smooth_radius);
  smooth_radius = 1;
  smoothPolarMatrix(cost_matrix, smooth_radius);
}

void getBestCandidatesFromCostMatrix(
    const Eigen::MatrixXd &matrix, unsigned int number_of_candidates,
    std::vector<candidateDirection> &candidate_vector) {
  std::priority_queue<candidateDirection, std::vector<candidateDirection>,
                      std::less<candidateDirection>>
      queue;

  for (int row_index = 0; row_index < matrix.rows(); row_index++) {
    for (int col_index = 0; col_index < matrix.cols(); col_index++) {
      double elevation_angle = elevationIndexToAngle(row_index, ALPHA_RES);
      double azimuth_angle = azimuthIndexToAngle(col_index, ALPHA_RES);
      double cost = matrix(row_index, col_index);
      candidateDirection candidate(cost, elevation_angle, azimuth_angle);

      if (queue.size() < number_of_candidates) {
        queue.push(candidate);
      } else if (candidate < queue.top()) {
        queue.push(candidate);
        queue.pop();
      }
    }
  }

  // copy queue to vector and change order such that lowest cost is at the front
  candidate_vector.clear();
  while (!queue.empty()) {
    candidate_vector.push_back(queue.top());
    queue.pop();
  }
  std::reverse(candidate_vector.begin(), candidate_vector.end());
}

void smoothPolarMatrix(Eigen::MatrixXd &matrix, unsigned int smoothing_radius) {
  // pad matrix by smoothing radius respecting all wrapping rules
  Eigen::MatrixXd matrix_padded;
  padPolarMatrix(matrix, smoothing_radius, matrix_padded);

  // filter matrix (max-mean)
  for (int row_index = smoothing_radius;
       row_index < matrix_padded.rows() - smoothing_radius; row_index++) {
    for (int col_index = smoothing_radius;
         col_index < matrix_padded.cols() - smoothing_radius; col_index++) {
      double original_val = matrix_padded(row_index, col_index);
      double mean_val =
          matrix_padded
              .block(row_index - smoothing_radius, col_index - smoothing_radius,
                     2 * smoothing_radius + 1, 2 * smoothing_radius + 1)
              .mean();
      matrix(row_index - smoothing_radius, col_index - smoothing_radius) =
          std::max(original_val, mean_val);
    }
  }
}

void padPolarMatrix(const Eigen::MatrixXd &matrix, unsigned int n_lines_padding,
                    Eigen::MatrixXd &matrix_padded) {
  matrix_padded.resize(matrix.rows() + 2 * n_lines_padding,
                       matrix.cols() + 2 * n_lines_padding);

  matrix_padded.fill(0.0);
  // middle part
  matrix_padded.block(n_lines_padding, n_lines_padding, matrix.rows(),
                      matrix.cols()) = matrix;

  if (matrix.cols() % 2 > 0) {
    ROS_ERROR("invalid resolution: 180 % (2* resolution) must be zero");
  }
  int middle_index = floor(matrix.cols() / 2);

  // top border
  matrix_padded.block(0, n_lines_padding, n_lines_padding, middle_index) =
      matrix.block(0, middle_index, n_lines_padding, middle_index)
          .colwise()
          .reverse();
  matrix_padded.block(0, n_lines_padding + middle_index, n_lines_padding,
                      middle_index) =
      matrix.block(0, 0, n_lines_padding, middle_index).colwise().reverse();

  // bottom border
  matrix_padded.block(matrix.rows() + n_lines_padding, n_lines_padding,
                      n_lines_padding, middle_index) =
      matrix
          .block(matrix.rows() - n_lines_padding, middle_index, n_lines_padding,
                 middle_index)
          .colwise()
          .reverse();
  matrix_padded.block(matrix.rows() + n_lines_padding,
                      n_lines_padding + middle_index, n_lines_padding,
                      middle_index) =
      matrix
          .block(matrix.rows() - n_lines_padding, 0, n_lines_padding,
                 middle_index)
          .colwise()
          .reverse();

  // left border
  matrix_padded.block(0, 0, matrix_padded.rows(), n_lines_padding) =
      matrix_padded.block(0, matrix_padded.cols() - 2 * n_lines_padding,
                          matrix_padded.rows(), n_lines_padding);
  // right border
  matrix_padded.block(0, n_lines_padding + matrix.cols(), matrix_padded.rows(),
                      n_lines_padding) =
      matrix_padded.block(0, n_lines_padding, matrix_padded.rows(),
                          n_lines_padding);
}

// costfunction for every free histogram cell
double costFunction(double e_angle, double z_angle, double obstacle_distance,
                    const Eigen::Vector3f &goal,
                    const Eigen::Vector3f &position,
                    const Eigen::Vector3f &last_sent_waypoint,
                    costParameters cost_params, bool only_yawed) {
  Eigen::Vector3f projected_candidate =
      fromPolarToCartesian(e_angle, z_angle, 1.0, toPoint(position));
  Eigen::Vector3f projected_goal = goal;
  Eigen::Vector3f projected_last_wp = last_sent_waypoint;

  if ((goal - position).norm() > 0.0001) {
    Eigen::Vector3f projected_goal = (goal - position).normalized();
  }
  if ((last_sent_waypoint - position).norm() > 0.0001) {
    Eigen::Vector3f projected_last_wp =
        (last_sent_waypoint - position).normalized();
  }

  // goal costs
  double yaw_cost =
      cost_params.goal_cost_param *
      (projected_goal.topRows<2>() - projected_candidate.topRows<2>()).norm();
  double pitch_cost_up = 0.0;
  double pitch_cost_down = 0.0;
  if (projected_candidate.z() > projected_goal.z()) {
    pitch_cost_up = cost_params.goal_cost_param *
                    std::abs(projected_goal.z() - projected_candidate.z());
  } else {
    pitch_cost_down = cost_params.goal_cost_param *
                      std::abs(projected_goal.z() - projected_candidate.z());
  }

  // smooth costs
  double yaw_cost_smooth =
      cost_params.smooth_cost_param *
      (projected_last_wp.topRows<2>() - projected_candidate.topRows<2>())
          .norm();
  double pitch_cost_smooth =
      cost_params.smooth_cost_param *
      std::abs(projected_last_wp.z() - projected_candidate.z());

  // distance cost
  double distance_cost = 0.0;
  if (obstacle_distance > 0) {
    distance_cost = 500 * 1.0 / obstacle_distance;
  }

  // combine costs
  double cost = 0.0;
  if (!only_yawed) {
    cost = yaw_cost +
           cost_params.height_change_cost_param_adapted * pitch_cost_up +
           cost_params.height_change_cost_param * pitch_cost_down +
           yaw_cost_smooth + pitch_cost_smooth + distance_cost;
  } else {
    cost = yaw_cost +
           cost_params.height_change_cost_param_adapted * pitch_cost_up +
           cost_params.height_change_cost_param * pitch_cost_down +
           0.5 * yaw_cost_smooth + 0.5 * pitch_cost_smooth + distance_cost;
  }

  return cost;
}

bool getDirectionFromTree(
    Eigen::Vector3f &p,
    const std::vector<geometry_msgs::Point> &path_node_positions,
    const Eigen::Vector3f &position) {
  int size = path_node_positions.size();
  bool tree_available = true;

  if (size > 0) {
    int min_dist_idx = 0;
    int second_min_dist_idx = 0;
    double min_dist = HUGE_VAL;
    double second_min_dist = HUGE_VAL;
    double node_distance =
        (toEigen(path_node_positions[0]) - toEigen(path_node_positions[1]))
            .norm();

    std::vector<double> distances;
    for (int i = 0; i < size; i++) {
      distances.push_back((position - toEigen(path_node_positions[i])).norm());
      if (distances[i] < min_dist) {
        second_min_dist_idx = min_dist_idx;
        second_min_dist = min_dist;
        min_dist = distances[i];
        min_dist_idx = i;
      } else if (distances[i] < second_min_dist) {
        second_min_dist = distances[i];
        second_min_dist_idx = i;
      }
    }
    int wp_idx = std::min(min_dist_idx, second_min_dist_idx);
    if (min_dist > 3.0) {
      tree_available = false;
    } else if (wp_idx == 0) {
      if (size == 2) {
        int wp_e = floor(elevationAnglefromCartesian(
            toEigen(path_node_positions[0]), position));
        int wp_z = floor(azimuthAnglefromCartesian(
            toEigen(path_node_positions[0]), position));
        p = Eigen::Vector3f(wp_e, wp_z, 0.f);
      } else {
        tree_available = false;
      }
    } else {
      double cos_alpha = (node_distance * node_distance +
                          distances[wp_idx] * distances[wp_idx] -
                          distances[wp_idx + 1] * distances[wp_idx + 1]) /
                         (2 * node_distance * distances[wp_idx]);
      double l_front = distances[wp_idx] * cos_alpha;
      double l_frac = l_front / node_distance;

      Eigen::Vector3f mean_point =
          (1.f - l_frac) * toEigen(path_node_positions[wp_idx - 1]) +
          l_frac * toEigen(path_node_positions[wp_idx]);

      int wp_e = floor(elevationAnglefromCartesian(mean_point, position));
      int wp_z = floor(azimuthAnglefromCartesian(mean_point, position));

      p = Eigen::Vector3f(wp_e, wp_z, 0.f);
    }
  } else {
    tree_available = false;
  }
  return tree_available;
}

void printHistogram(Histogram &histogram) {
  std::cout << "------------------------------------------Histogram------------"
               "------------------------------------\n";
  for (int e = 0; e < GRID_LENGTH_E; e++) {
    for (int z = 0; z < GRID_LENGTH_Z; z++) {
      int val = floor(histogram.get_dist(e, z));
      if (val > 99) {
        std::cout << val << " ";
      } else if (val > 9) {
        std::cout << val << "  ";
      } else {
        std::cout << val << "   ";
      }
    }
    std::cout << "\n";
  }
  std::cout << "_______________________________________________________________"
               "____________________________________\n";
}
}
