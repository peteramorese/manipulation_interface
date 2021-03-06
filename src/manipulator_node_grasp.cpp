#include "ros/ros.h"
#include <ros/callback_queue.h>
#include <iostream>
#include "geometry_msgs/PoseStamped.h"
#include "manipulation_interface/PlanningQuery.h"
#include <vector>
#include <cmath>

#include <pluginlib/class_loader.h>
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/planning_interface/planning_interface.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/kinematic_constraints/utils.h>
#include <moveit_msgs/PlanningScene.h>
#include <boost/scoped_ptr.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <moveit/move_group_interface/move_group_interface.h>
#include <moveit/planning_scene_interface/planning_scene_interface.h>
#include <moveit/trajectory_processing/iterative_time_parameterization.h>
#include <moveit_msgs/DisplayRobotState.h>
#include <moveit_msgs/DisplayTrajectory.h>
#include <moveit_msgs/AttachedCollisionObject.h>
#include <moveit_msgs/CollisionObject.h>
#include <moveit_visual_tools/moveit_visual_tools.h>

#include "franka_gripper/GraspAction.h"
#include "franka_gripper/GraspActionGoal.h"

static const std::string NONE = "none";
static const std::string PLANNING_GROUP = "panda_arm";

class PlanningQuerySrv {
	private:
		moveit::planning_interface::MoveGroupInterface* move_group_ptr;
		moveit::planning_interface::PlanningSceneInterface* planning_scene_interface_ptr;
		actionlib::SimpleActionClient<franka_gripper::GraspAction>* grp_act_ptr;
		std::vector<moveit_msgs::CollisionObject> col_obj_vec;
		std::vector<std::string> obs_domain_labels;
		franka_gripper::GraspActionGoal grip_goal;
		bool use_gripper;
		std::string attached_obj;
		//struct grasp_mode {
		//	std::string mode;
		//	tf2::Quaternion rotation;
		//};
		//std::vector<grasp_mode> grasp_modes;
		//int current_mode;
		std::string current_grasp_mode;
		geometry_msgs::Pose prev_pose;
	public:
		PlanningQuerySrv(moveit::planning_interface::MoveGroupInterface* move_group_ptr_, moveit::planning_interface::PlanningSceneInterface* psi_ptr_, actionlib::SimpleActionClient<franka_gripper::GraspAction>* grp_act_ptr_,  int N_TRIALS_, bool use_gripper_) :
			move_group_ptr(move_group_ptr_),
			planning_scene_interface_ptr(psi_ptr_),
			grp_act_ptr(grp_act_ptr_),
			N_TRIALS(N_TRIALS_),
			use_gripper(use_gripper_) {
				attached_obj = NONE;
				current_grasp_mode = NONE;
				//grasp_modes.clear();
				if (use_gripper) {
					ROS_INFO_NAMED("manipulator_node","Waiting for franka_gripper action to show...");
					grp_act_ptr->waitForServer();
					ROS_INFO_NAMED("manipulator_node","Found franka_gripper action.");
				}
			}
		// DEFINE CONSTANTS FOR PROBLEM
		const double bag_l = .045;//.045;
		const double bag_w = .07;
		const double bag_h = .157;
		const double eef_offset = .075;
		const double grip_width_closed = .044;
		const double grip_width_open = .1;
		const double grip_speed = .1;
		const double grip_force = 50;
		const double grip_epsilon_inner = .03;
		const double grip_epsilon_outer = .03;
		const double approach_dist = .3;
		const double jump_thresh = 0.0;
		const double eef_step = 0.1;
		const int num_waypts = 3;
		const double max_acceleration_scale = 0.1;
		const int N_TRIALS;
		int pourCounter = 0;
		void setWorkspace(std::vector<moveit_msgs::CollisionObject> col_obj_vec_ws, std::vector<std::string> col_obj_vec_dom_lbls) {
			col_obj_vec.clear();
			obs_domain_labels.clear();
			col_obj_vec = col_obj_vec_ws;
			obs_domain_labels = col_obj_vec_dom_lbls;
		}
		//void addMode(std::string mode_, tf2::Quaternion rotation_) {
		//	if (mode_ != NONE) {
		//		grasp_mode temp_mode;
		//		temp_mode.rotation = rotation_;
		//		temp_mode.mode = mode_;
		//		grasp_modes.push_back(temp_mode);
		//	} else {
		//		ROS_ERROR_NAMED("manipulator_node", "Cannot set 'mode' to 'none'");
		//	}
		//}

		void setupEnvironment(std::string planning_domain_lbl) {
			std::cout<<"recieved planning domain label in setupEnvironment: "<<planning_domain_lbl<<std::endl;
			std::cout<<"obs_domain_labels size: "<<obs_domain_labels.size()<<std::endl;
			std::vector<moveit_msgs::CollisionObject> temp_col_vec;
			for (int i=0; i<col_obj_vec.size(); ++i) {
				if (obs_domain_labels[i] == "none") {
					ROS_INFO_NAMED("manipulator_node", "Environment Setup: Ignoring object with label: %s", col_obj_vec[i].id.c_str());
				} else if (obs_domain_labels[i] == planning_domain_lbl) {
					std::cout<<"adding:"<< col_obj_vec[i].id<<std::endl;
					col_obj_vec[i].operation = col_obj_vec[i].ADD;
					temp_col_vec.push_back(col_obj_vec[i]);
				} else {
					std::cout<<"removing:"<< col_obj_vec[i].id<<std::endl;
					col_obj_vec[i].operation = col_obj_vec[i].REMOVE;
					temp_col_vec.push_back(col_obj_vec[i]);
				}
			}
			planning_scene_interface_ptr->applyCollisionObjects(temp_col_vec);
		}

		void findObjAndUpdate(std::string obj_id, std::string domain_label_) {
			bool not_found = true;
			for (int i=0; i<col_obj_vec.size(); ++i) {
				if (col_obj_vec[i].id == obj_id) {
					std::cout<<"FIND UPDATE: found object id: "<<obj_id<<std::endl;
					std::cout<<"FIND UPDATE: updating to domain label: "<<domain_label_<<std::endl;
					obs_domain_labels[i] = domain_label_;
					not_found = false;
				}
			}
			if (not_found) {
				ROS_ERROR_NAMED("manipulator_node","Object id was not found. Cannot update domain label");
			}
		}

		bool planQuery_serviceCB(manipulation_interface::PlanningQuery::Request &request, manipulation_interface::PlanningQuery::Response &response) {
			std::cout<<"\n";
			std::cout<<"HELLO"<<std::endl;
			std::cout<<"Recieved planning domain: "<<request.planning_domain<<std::endl;
			std::cout<<"\n";
			ROS_INFO_NAMED("manipulator_node", "Recieved Planning Query");

			trajectory_processing::IterativeParabolicTimeParameterization IPTP;
			robot_trajectory::RobotTrajectory r_trajectory(move_group_ptr->getRobotModel(), PLANNING_GROUP);

			if (request.setup_environment) {
				//col_obj_vec.resize(request.bag_poses.size());
				//bag_domain_labels = request.bag_domain_labels;

				for (int i=0; i<request.bag_poses.size(); ++i) {
					moveit_msgs::CollisionObject temp_col_obj;
					temp_col_obj.header.frame_id = "panda_link0";
					temp_col_obj.id = request.bag_labels[i];
					temp_col_obj.primitives.resize(1);
					temp_col_obj.primitives[0].type = col_obj_vec[i].primitives[0].BOX;
					temp_col_obj.primitives[0].dimensions.resize(3);
					temp_col_obj.primitives[0].dimensions[0] = bag_l;
					temp_col_obj.primitives[0].dimensions[1] = bag_w;
					temp_col_obj.primitives[0].dimensions[2] = bag_h;

					temp_col_obj.primitive_poses.resize(1);
					std::cout<<" i see : "<<request.bag_poses[i].position.x<<std::endl;
					std::cout<<" i see : "<<request.bag_poses[i].position.y<<std::endl;
					std::cout<<" i see : "<<request.bag_poses[i].position.z<<std::endl;
					temp_col_obj.primitive_poses[0].position.x = request.bag_poses[i].position.x;
					temp_col_obj.primitive_poses[0].position.y = request.bag_poses[i].position.y;
					temp_col_obj.primitive_poses[0].position.z = request.bag_poses[i].position.z;
					temp_col_obj.primitive_poses[0].orientation.x = request.bag_poses[i].orientation.x;
					temp_col_obj.primitive_poses[0].orientation.y = request.bag_poses[i].orientation.y;
					temp_col_obj.primitive_poses[0].orientation.z = request.bag_poses[i].orientation.z;
					temp_col_obj.primitive_poses[0].orientation.w = request.bag_poses[i].orientation.w;
					col_obj_vec.push_back(temp_col_obj);
					obs_domain_labels.push_back(request.bag_domain_labels[i]);
					std::cout<<"Adding object: "<<temp_col_obj.id<<" to domain: "<<request.bag_domain_labels[i]<<std::endl;
					//col_obj_vec[i].operation = col_obj_vec[i].ADD;
				}
				setupEnvironment(request.planning_domain);
			}


			if (request.pickup_object != "none") {
				ROS_INFO_NAMED("manipulator_node","Working on grasp...");

				// MOVE FROM THE TOP TO THE MIDDLE OF AN OBJECT

				std::vector<geometry_msgs::Pose> waypts(num_waypts);
				waypts[0] = move_group_ptr->getCurrentPose().pose;
				waypts[num_waypts-1] = move_group_ptr->getCurrentPose().pose;
				for (int i=1; i<num_waypts; ++i) {
					waypts[i] = prev_pose;
					waypts[i].position.z = waypts[i].position.z + approach_dist/num_waypts*(num_waypts - 1 - i);
				}
				moveit_msgs::RobotTrajectory trajectory;
				double fraction = move_group_ptr->computeCartesianPath(waypts, eef_step, jump_thresh, trajectory);
				//std::vector<double> time_diff;
				//for (int i=0; i<num_waypts; ++i) {
				//	time_diff[i] = 1.0;
				//}
				//const double v_scaling = .02;
				r_trajectory.setRobotTrajectoryMsg(*(move_group_ptr->getCurrentState()), trajectory);
				IPTP.computeTimeStamps(r_trajectory, max_acceleration_scale, max_acceleration_scale); // max_acceleration_scale
				moveit_msgs::RobotTrajectory r_trajectory_msg;
				r_trajectory.getRobotTrajectoryMsg(r_trajectory_msg);
				move_group_ptr->setMaxVelocityScalingFactor(1);
				move_group_ptr->execute(r_trajectory_msg);

				// GRASP AN OBJECT

				std::string obj_label = request.pickup_object;
				move_group_ptr->attachObject(obj_label,"panda_link8");
				// Change the domain of the attached object to be 'none'
				// so that it does not get removed
				findObjAndUpdate(obj_label, "none");
				attached_obj = obj_label;
				ROS_INFO_NAMED("manipulator_node","Grabbing object...");
				if (use_gripper) {
					grip_goal.goal.width = grip_width_closed; // Use closed width
					grip_goal.goal.speed = grip_speed;
					grip_goal.goal.force = grip_force;
					grip_goal.goal.epsilon.inner = grip_epsilon_inner;
					grip_goal.goal.epsilon.outer = grip_epsilon_outer;
					grp_act_ptr->sendGoal(grip_goal.goal);
					grp_act_ptr->waitForResult(ros::Duration(5.0));
				}

				// RETURN BACK

				std::vector<geometry_msgs::Pose> waypts_rev(num_waypts);
				waypts_rev[0] = move_group_ptr->getCurrentPose().pose;
				for (int i=1; i<num_waypts; ++i) {
					waypts_rev[i] = waypts[num_waypts-1-i];
					//waypts_rev[i] = prev_pose;
					//waypts_rev[i].position.z = waypts[i].position.z + approach_dist/num_waypts*(i+1);
				}
				fraction = move_group_ptr->computeCartesianPath(waypts_rev, eef_step, jump_thresh, trajectory, false);
				r_trajectory.setRobotTrajectoryMsg(*(move_group_ptr->getCurrentState()), trajectory);
				IPTP.computeTimeStamps(r_trajectory, max_acceleration_scale, max_acceleration_scale); // max_acceleration_scale
				r_trajectory.getRobotTrajectoryMsg(r_trajectory_msg);
				move_group_ptr->setMaxVelocityScalingFactor(1);
				move_group_ptr->execute(r_trajectory_msg);

				// Set the grasp mode once the object has been picked up
				//bool found = false;
				//for (int i=0; i<grasp_modes.size(); ++i) {
				//	if (grasp_modes[i].mode == current_grasp_mode) {
				//		found = true;
				//		mode =
				//	}
				//}
				ROS_INFO_NAMED("manipulator_node","Done grabbing object");
				response.success = true;
			} else if (request.drop_object != "none") {

				// MOVE FROM THE TOP TO THE MIDDLE OF AN OBJECT

				//move_group_ptr->setMaxVelocityScalingFactor(.02);
				std::vector<geometry_msgs::Pose> waypts(num_waypts);
				waypts[0] = move_group_ptr->getCurrentPose().pose;
				for (int i=1; i<num_waypts; ++i) {
					waypts[i] = prev_pose;
					waypts[i].position.z = waypts[i].position.z + approach_dist/num_waypts*(num_waypts - 1 - i);
				}
				moveit_msgs::RobotTrajectory trajectory;
				double fraction = move_group_ptr->computeCartesianPath(waypts, eef_step, jump_thresh, trajectory);
				r_trajectory.setRobotTrajectoryMsg(*(move_group_ptr->getCurrentState()), trajectory);
				IPTP.computeTimeStamps(r_trajectory, max_acceleration_scale, max_acceleration_scale); // max_acceleration_scale
				moveit_msgs::RobotTrajectory r_trajectory_msg;
				r_trajectory.getRobotTrajectoryMsg(r_trajectory_msg);
				move_group_ptr->setMaxVelocityScalingFactor(1);
				move_group_ptr->execute(r_trajectory_msg);

				// RELEASE AN OBJECT
				std::string obj_label = request.drop_object;
				move_group_ptr->detachObject(obj_label);

				std::vector<std::string>::iterator itr = std::find(request.bag_labels.begin(), request.bag_labels.end(), request.drop_object);
    			if (itr != request.bag_labels.cend()) {
        			int idx = std::distance(request.bag_labels.begin(), itr);
					col_obj_vec[idx+2].primitive_poses[0] = request.manipulator_pose;
    			}
    			else {
        			std::cout << request.drop_object << " not found" << std::endl;
    			}
				// If we are releasing an object, the new domain becomes
				// whatever domain the end effector is in (the request)
				findObjAndUpdate(obj_label, request.planning_domain);
				attached_obj = "none";
				ROS_INFO_NAMED("manipulator_node","Dropping object...");
				if (use_gripper) {
					grip_goal.goal.width = grip_width_open; // Use open width
					grip_goal.goal.speed = grip_speed;
					grip_goal.goal.force = grip_force;
					grip_goal.goal.epsilon.inner = grip_epsilon_inner;
					grip_goal.goal.epsilon.outer = grip_epsilon_outer;
					grp_act_ptr->sendGoal(grip_goal.goal);
					grp_act_ptr->waitForResult(ros::Duration(5.0));
				}

				// RETURN BACK

				std::vector<geometry_msgs::Pose> waypts_rev(num_waypts);
				waypts_rev[0] = move_group_ptr->getCurrentPose().pose;
				for (int i=1; i<num_waypts; ++i) {
					waypts_rev[i] = waypts[num_waypts-1-i];
				}
				fraction = move_group_ptr->computeCartesianPath(waypts_rev, eef_step, jump_thresh, trajectory);
				r_trajectory.setRobotTrajectoryMsg(*(move_group_ptr->getCurrentState()), trajectory);
				IPTP.computeTimeStamps(r_trajectory, max_acceleration_scale, max_acceleration_scale);
				r_trajectory.getRobotTrajectoryMsg(r_trajectory_msg);
				move_group_ptr->setMaxVelocityScalingFactor(1);
				move_group_ptr->execute(r_trajectory_msg);

				setupEnvironment(request.planning_domain);
				response.success = true;
				ROS_INFO_NAMED("manipulator_node","Done dropping object");

			} else if (request.grasp_type == "mode" && current_grasp_mode == "side") {

				// Setup the environment objects in rviz
				setupEnvironment(request.planning_domain);

				geometry_msgs::Pose curr_pose = move_group_ptr->getCurrentPose().pose;

				// THIS SECTION IS ONLY FOR "SIDE GRASP AND TILT MODEEEEEEEEEEEEEEEE"
				geometry_msgs::Point center;
				if (request.to_loc == "G0" || request.to_loc == "G1" || request.to_loc == "G2") {
					center.x = 0.3;
					center.y = 0.00;
					center.z = 0.1;
				} else if (request.to_loc == "L0") {
					center.x = 0.3;
					center.y = 0.48;
					center.z = 0.1;
				} else if (request.to_loc == "L1") {
					center.x = 0.35;
					center.y = -0.35;
					center.z = 0.1;
				} else if (request.to_loc == "L2") {
					center.x = 0.01;
					center.y = 0.3;
					center.z = 0.1;
				}

				std::cout << "======================== MODE AND SIDE ========================"<< std::endl;
				std::vector<geometry_msgs::Pose> poses;

				tf2::Quaternion q_pos_offset, q_neg_offset;
				std::vector<tf2::Quaternion> q_f;
				std::vector<tf2::Quaternion> q_rot;

				int N_grasps = 2;
				// int N_grasps = 1;
				q_f.resize(N_grasps);
				q_rot.resize(N_grasps);
				poses.resize(N_grasps);

				q_pos_offset[0] = 0;
				q_pos_offset[1] = (bag_w/2 + eef_offset);
				q_pos_offset[2] = 0;
				q_pos_offset[3] = 0;
				q_neg_offset = q_pos_offset;
				q_neg_offset[1] = -q_neg_offset[1];

				// Find the ee's direction
				tf2::Quaternion q_z, q_rotated_z, q_curr;
				tf2::convert(curr_pose.orientation, q_curr);
				// Define a z axis
				q_z[0] = 0;
				q_z[1] = 0;
				q_z[2] = 1;
				q_z[3] = 0;
				// Rotate by the ee quaternion to get ee's direction
				q_rotated_z = q_curr * q_z * q_curr.inverse();

				tf2::Quaternion q_flip;
				q_flip.setRPY(0, 0, M_PI);
				if (q_rotated_z[1] >= 0) {
					q_f[0] = q_neg_offset;
					q_rot[0] = q_curr;
					q_f[1] = q_pos_offset;
					q_rot[1] = q_flip * q_curr;
				} else {
					q_f[0] = q_pos_offset;
					q_rot[0] = q_curr;
					q_f[1] = q_neg_offset;
					q_rot[1] = q_flip * q_curr;
				}

				for (int ii=0; ii<poses.size(); ++ii) {
					poses[ii].position.x = center.x + q_f[ii][0];
					poses[ii].position.y = center.y + q_f[ii][1];
					poses[ii].position.z = center.z + q_f[ii][2];
					q_rot[ii].normalize();
					poses[ii].orientation.x = q_rot[ii][0];
					poses[ii].orientation.y = q_rot[ii][1];
					poses[ii].orientation.z = q_rot[ii][2];
					poses[ii].orientation.w = q_rot[ii][3];
				}

				move_group_ptr->setPlanningTime(5.0);

				moveit_msgs::OrientationConstraint ocm;
				ocm.link_name = "panda_link8";
				ocm.header.frame_id = "panda_link0";
				ocm.orientation = move_group_ptr->getCurrentPose().pose.orientation;
				ocm.absolute_x_axis_tolerance = 0.1;
				ocm.absolute_y_axis_tolerance = 0.1;
				ocm.absolute_z_axis_tolerance = 1.0;
				ocm.weight = 1.0;
				moveit_msgs::Constraints test_constraints;
				test_constraints.orientation_constraints.push_back(ocm);

				bool success = false;
				bool success_ex = false;
				for (int ii=0; ii<N_TRIALS; ii++){
					moveit::planning_interface::MoveGroupInterface::Plan plan_;

					move_group_ptr->setStartStateToCurrentState();
					for (int i_pose=0; i_pose<poses.size(); ++i_pose) {
						geometry_msgs::Pose p = poses[i_pose];
						p.position.z += approach_dist;

						if (request.to_loc == "L0" || request.to_loc == "L1" || request.to_loc == "L2") {
							p.position.x += (rand() % 10 - 5) / 100.0;
							p.position.y += (rand() % 10 - 5) / 100.0;
							// p.position.y += approach_dist;
						}

						std::cout<<" --- Working on transfer: "<<i_pose<<" --- "<<std::endl;
						std::cout << p << std::endl;

						move_group_ptr->setPoseTarget(p);
						// if (i_pose == 0) move_group_ptr->setPathConstraints(test_constraints);
						ros::WallDuration(1.0).sleep();
						success = (move_group_ptr->plan(plan_)==moveit::planning_interface::MoveItErrorCode::SUCCESS);
						if (success){
							std::cout<<"Plan test succeeded!"<<std::endl;
							prev_pose = poses[i_pose];
							prev_pose.position.x += request.manipulator_pose.position.x - center.x;
							prev_pose.position.y += request.manipulator_pose.position.y - center.y;
							prev_pose.position.z += request.manipulator_pose.position.z - center.z;
							if (request.go_to_raised) {
								geometry_msgs::Pose raised_pose = poses[i_pose];
								raised_pose.position.z = raised_pose.position.z + approach_dist;
								move_group_ptr->setPoseTarget(raised_pose);
								success_ex = (move_group_ptr->plan(plan_)==moveit::planning_interface::MoveItErrorCode::SUCCESS);
							}
							ROS_INFO_NAMED("manipulator_node","Completed planning on iteration: %d",ii);
							move_group_ptr->setMaxVelocityScalingFactor(1);
							move_group_ptr->execute(plan_);
							break;
						}

						move_group_ptr->clearPathConstraints();
					}
					if (success_ex) {
						break;
					}
					ros::WallDuration(1.0).sleep();
				}
				move_group_ptr->clearPathConstraints();

				if (request.to_loc == "L0" || request.to_loc == "L1" || request.to_loc == "L2") {
					std::cout << "======================== SUCCESS ========================"<< std::endl;
					response.success = success;
					ros::WallDuration(1.0).sleep();
					return true;
				}

				std::cout << "======================== TILTING ========================"<< std::endl;

				geometry_msgs::Pose ee_pose;
				tf2::Quaternion q_tilt, q_ee;

				// 5. Define way points
				std::vector<geometry_msgs::Pose> waypts(3);
				moveit_msgs::RobotTrajectory trajectory;
				// Start from the current pose and end at the current pose
				waypts[0] = move_group_ptr->getCurrentPose().pose;
				waypts[2] = waypts[0];
				ee_pose = waypts[0];

				// 6. Add a Tilting pose
				tf2::convert(ee_pose.orientation, q_curr);
				// Rotate by the ee quaternion to get ee's direction
				q_rotated_z = q_curr * q_z * q_curr.inverse();

				if (q_rotated_z[1] >= 0) {
					q_tilt.setRPY(-M_PI/3, 0, 0);
				} else {
					q_tilt.setRPY(M_PI/3, 0, 0);
				}
				q_ee = q_tilt * q_curr;
				q_ee.normalize();
				ee_pose.orientation.x = q_ee[0];
				ee_pose.orientation.y = q_ee[1];
				ee_pose.orientation.z = q_ee[2];
				ee_pose.orientation.w = q_ee[3];
				ee_pose.position.z -= 0.1;
				waypts[1] = ee_pose;

				// 8. Plan & Execute
				double fraction = move_group_ptr->computeCartesianPath(waypts, eef_step, jump_thresh, trajectory);
				r_trajectory.setRobotTrajectoryMsg(*(move_group_ptr->getCurrentState()), trajectory);
				IPTP.computeTimeStamps(r_trajectory, max_acceleration_scale, max_acceleration_scale); // max_acceleration_scale
				moveit_msgs::RobotTrajectory r_trajectory_msg;
				r_trajectory.getRobotTrajectoryMsg(r_trajectory_msg);
				move_group_ptr->setMaxVelocityScalingFactor(1);
				move_group_ptr->execute(r_trajectory_msg);
				ros::WallDuration(1.0).sleep();

				if (request.to_loc == "G0") pourCounter = pourCounter + 1;

				std::cout << "======================== SUCCESS ========================"<< std::endl;
				response.success = success;

			} else {
				setupEnvironment(request.planning_domain);
				std::cout<<"moving"<<std::endl;
				std::vector<geometry_msgs::Pose> poses;
				moveit::planning_interface::MoveGroupInterface::Plan plan;

				// Convert from PoseStamed to Pose
				if (request.safe_config) {
					std::vector<double> joint_val_target = {0.0, -.785398, 0.0, -2.35619, 1.5708, .785398};
					move_group_ptr->setJointValueTarget(joint_val_target);
				} else {
					tf2::Quaternion q_orig, q_in, q_set;
					std::vector<tf2::Quaternion> q_f;
					std::vector<tf2::Quaternion> q_rot;
					// This quaternion sets the panda gripper to face down towards the
					// object grabbing along its length
					q_set.setRPY(0, M_PI, -M_PI/4 + M_PI/2);
					tf2::convert(request.manipulator_pose.orientation, q_in);

					std::string grasp_type = request.grasp_type;
					std::cout<<" Grasp Type: "<<grasp_type<<std::endl;

					if (grasp_type == "mode") {
						std::cout << "Using current grasp mode: " << current_grasp_mode << std::endl;
						grasp_type = current_grasp_mode;
					} else {
						// If the user does not use the current mode grasp, then
						// keep track of the specified grasp type to set the mode
						current_grasp_mode = grasp_type;
					}
					if (grasp_type == "up") {
						int N_grasps = 1;
						q_f.resize(N_grasps);
						q_rot.resize(N_grasps);
						poses.resize(N_grasps);

						q_orig[0] = 0;
						q_orig[1] = 0;
						q_orig[2] = bag_h/2 + eef_offset;
						q_orig[3] = 0;
						// Rotate the q
						q_f[0] = q_in * q_orig * q_in.inverse(); // TODO: No need for rotation.
						q_rot[0] = q_in * q_set; // TODO: Same here. It's always pointing the z axis

					} else if (grasp_type == "side") {
						int N_grasps = 2;
						q_f.resize(N_grasps);
						q_rot.resize(N_grasps);
						poses.resize(N_grasps);

						//q_orig[0] = 0;
						//q_orig[1] = bag_w/2 + eef_offset;
						//q_orig[2] = 0;
						//q_orig[3] = 0;
						q_orig[0] = 0;
						q_orig[1] = -(bag_w/2 + eef_offset);
						q_orig[2] = 0;
						q_orig[3] = 0;
						// 90 degrees
						tf2::Quaternion q_set_2;
						{
							tf2::Quaternion q_set_temp;
							q_set_temp.setRPY(M_PI/2, 0, 0);
							q_set_2 = q_set_temp * q_set;
						}
						q_f[0] = q_in * q_orig * q_in.inverse();// TODO: No need for rotation.
						q_rot[0] = q_in * q_set_2;

						// -90 degrees
						{
							tf2::Quaternion q_set_temp;
							q_set_temp.setRPY(-M_PI/2, 0, 0);
							q_set_2 = q_set_temp * q_set;
						}
						q_orig[1] = -q_orig[1]; //flip the axis
						q_f[1] = q_in * q_orig * q_in.inverse();// TODO: No need for rotation.
						q_rot[1] = q_in * q_set_2;

					} else if (grasp_type == NONE) {
						ROS_ERROR_NAMED("manipulator_node","Sent transfer action before sending transit action. Cannot resolve grasp type");
					} else {
						ROS_ERROR_NAMED("manipulator_node","Unrecognized grasp type");
					}

					for (int ii=0; ii<poses.size(); ++ii) {
						poses[ii].position.x = request.manipulator_pose.position.x + q_f[ii][0];
						poses[ii].position.y = request.manipulator_pose.position.y + q_f[ii][1];
						poses[ii].position.z = request.manipulator_pose.position.z + q_f[ii][2];
						q_rot[ii].normalize();
						poses[ii].orientation.x = q_rot[ii][0];
						poses[ii].orientation.y = q_rot[ii][1];
						poses[ii].orientation.z = q_rot[ii][2];
						poses[ii].orientation.w = q_rot[ii][3];
					}
					//move_group_ptr->setPoseTarget(pose);
				}
				move_group_ptr->setPlanningTime(5.0);

				//ROS_INFO_NAMED("manipulator_node", "Reference frame: %s", move_group_ptr->getPlanningFrame().c_str());
				//std::cout<<"moving to x: "<< pose.position.x<<std::endl;
				//std::cout<<"moving to y: "<< pose.position.y<<std::endl;
				//std::cout<<"moving to z: "<< pose.position.z<<std::endl;
				//std::cout<<"moving to qx: "<< pose.orientation.x<<std::endl;
				//std::cout<<"moving to qy: "<< pose.orientation.y<<std::endl;
				//std::cout<<"moving to qz: "<< pose.orientation.z<<std::endl;
				//std::cout<<"moving to qw: "<< pose.orientation.w<<std::endl;
				bool success = false;
				bool success_ex = false;
				for (int ii=0; ii<N_TRIALS; ii++){
					moveit::planning_interface::MoveGroupInterface::Plan plan_;

					move_group_ptr->setStartStateToCurrentState();
					for (int iii=0; iii<poses.size(); ++iii) {
						std::cout<<" --- Working on transit: "<<iii<<" --- "<<std::endl;
						std::cout << poses[iii] << std::endl;

						move_group_ptr->setPoseTarget(poses[iii]);
						ros::WallDuration(1.0).sleep();
						success = (move_group_ptr->plan(plan_)==moveit::planning_interface::MoveItErrorCode::SUCCESS);
						if (success){
							std::cout<<"Plan test succeeded!"<<std::endl;
							prev_pose = poses[iii];
							if (request.go_to_raised) {
								geometry_msgs::Pose raised_pose = poses[iii];
								raised_pose.position.z = raised_pose.position.z + approach_dist;
								move_group_ptr->setPoseTarget(raised_pose);
								success_ex = (move_group_ptr->plan(plan_)==moveit::planning_interface::MoveItErrorCode::SUCCESS);
							}
							ROS_INFO_NAMED("manipulator_node","Completed planning on iteration: %d",ii);
							move_group_ptr->execute(plan_);
							break;
						}
					}
					if (success) {
						break;
					}
					ros::WallDuration(1.0).sleep();
				}
				response.success = success;
				std::cout<<"done moving"<<std::endl;
			}
			return true;
		}

};


int main(int argc, char **argv) {

	ros::init(argc, argv, "manipulator_node");
	ros::NodeHandle M_NH("~");

	ros::AsyncSpinner spinner(2);
	spinner.start();

	bool sim_only, mock_observer;
	M_NH.getParam("sim_only", sim_only);
	M_NH.getParam("mock_observer", mock_observer);



	/////////////////////////////////////////////////////////////////////////
	actionlib::SimpleActionClient<franka_gripper::GraspAction> grip_client("/franka_gripper/grasp", true);
	franka_gripper::GraspActionGoal grip_goal;

	/////////////////////////////////////////////////////////////////////////

	moveit::planning_interface::MoveGroupInterface move_group(PLANNING_GROUP);

	moveit::planning_interface::PlanningSceneInterface planning_scene_interface;
	robot_model_loader::RobotModelLoader robot_model_loader("robot_description");
	robot_model::RobotModelPtr robot_model = robot_model_loader.getModel();

	robot_state::RobotStatePtr robot_state(new robot_state::RobotState(robot_model));
	const robot_state::JointModelGroup* joint_model_group = robot_state->getJointModelGroup(PLANNING_GROUP);

	planning_scene::PlanningScenePtr planning_scene(new planning_scene::PlanningScene(robot_model));
	planning_scene->getCurrentStateNonConst().setToDefaultValues(joint_model_group, "ready");
	moveit_visual_tools::MoveItVisualTools visual_tools("panda_link0");
	visual_tools.deleteAllMarkers();
	visual_tools.trigger();



	//LOADING A PLANNER
	boost::scoped_ptr<pluginlib::ClassLoader<planning_interface::PlannerManager>> planner_plugin_loader;
	planning_interface::PlannerManagerPtr planner_instance;
	std::string planner_plugin_name;

	//from tutorial
	M_NH.getParam("planning_plugin", planner_plugin_name);
	std::cout<<"\n\n\n\n"<<planner_plugin_name<<std::endl;
	if (!M_NH.getParam("planning_plugin", planner_plugin_name))
		ROS_FATAL_STREAM("Could not find planner plugin name");
	try
	{
		planner_plugin_loader.reset(new pluginlib::ClassLoader<planning_interface::PlannerManager>(
					"moveit_core", "planning_interface::PlannerManager"));
	}
	catch (pluginlib::PluginlibException& ex)
	{
		ROS_FATAL_STREAM("Exception while creating planning plugin loader " << ex.what());
	}
	try
	{
		planner_instance.reset(planner_plugin_loader->createUnmanagedInstance(planner_plugin_name));
		if (!planner_instance->initialize(robot_model, M_NH.getNamespace()))
			ROS_FATAL_STREAM("Could not initialize planner instance");
		ROS_INFO_STREAM("Using planner '" << planner_instance->getDescription() << "'");
	}

	catch (pluginlib::PluginlibException& ex)
	{
		const std::vector<std::string>& classes = planner_plugin_loader->getDeclaredClasses();
		std::stringstream ss;
		for (std::size_t i = 0; i < classes.size(); ++i)
			ss << classes[i] << " ";
		ROS_ERROR_STREAM("Exception while loading planner '" << planner_plugin_name << "': " << ex.what() << std::endl
				<< "Available plugins: " << ss.str());
	}
	/////////////////////////////////////////////////////////////////////////

	move_group.setPlannerId("RRTConnectkConfigDefault");
	ROS_INFO_NAMED("manipulator_node", "Reference frame: %s", move_group.getPlanningFrame().c_str());

	std::vector<moveit_msgs::CollisionObject> colObjVec;
	std::vector<std::string> colObjVec_domain_lbls;
	//colObjVec.resize(Nobj);
	//colObjVec_domain_lbls.resize(Nobj);
	int ind = 0;
	/*
	   for (int i=0; i<Nobj; i++) {
	   colObjVec[i].header.frame_id = "base_link";
	   }
	   */


	/* Define Table, Collision Environment, End Effector */

	// ground:
	/*
	ground.header.frame_id = "world";
	ground.id = "ground_p";
	ground.primitives.resize(1);
	ground.primitives[0].type = colObjVec[0].primitives[0].BOX;
	ground.primitives[0].dimensions.resize(3);
	ground.primitives[0].dimensions[0] = 2;
	ground.primitives[0].dimensions[1] = 2.4;
	ground.primitives[0].dimensions[2] = .1;

	ground.primitive_poses.resize(1);
	ground.primitive_poses[0].position.x = 0;
	ground.primitive_poses[0].position.y = 0;
	ground.primitive_poses[0].position.z = -.05; // should be -.5
	ground.primitive_poses[0].orientation.x = 0;
	ground.primitive_poses[0].orientation.y = 0;
	ground.primitive_poses[0].orientation.z = 0;
	ground.primitive_poses[0].orientation.w = 1;
	ground.operation = ground.ADD;
	colObjVec.push_back(ground);
	colObjVec_domain_lbls.push_back("pickup domain");

	ground.id = "ground_d";
	*/

	//moveit_msgs::CollisionObject ground;
 //       float x_bound, y_bound;
 //       x_bound = .1;
 //       y_bound = .1;
 //       ground.header.frame_id = "panda_link0";
 //       ground.id = "ground";
 //       //colObjIDs.push_back(ground.id);
 //       ground.primitives.resize(4);
 //       ground.primitives[0].type = ground.primitives[0].BOX;
 //       ground.primitives[0].dimensions.resize(3);
 //       ground.primitives[0].dimensions[0] = 1;
 //       ground.primitives[0].dimensions[1] = 2;
 //       ground.primitives[0].dimensions[2] = .05;
 //       ground.primitive_poses.resize(4);
 //       ground.primitive_poses[0].orientation.w = 1;
 //       ground.primitive_poses[0].position.x = 0.50 + x_bound;
 //       ground.primitive_poses[0].position.y = 0;
 //       ground.primitive_poses[0].position.z = -.05/2;
 //       ground.primitives[1].type = ground.primitives[0].BOX;
 //       ground.primitives[1].dimensions.resize(3);
 //       ground.primitives[1].dimensions[0] = 1;
 //       ground.primitives[1].dimensions[1] = 2;
 //       ground.primitives[1].dimensions[2] = .05;
 //       ground.primitive_poses[1].orientation.w = 1;
 //       ground.primitive_poses[1].position.x = -(.50 + x_bound);
 //       ground.primitive_poses[1].position.y = 0;
 //       ground.primitive_poses[1].position.z = -.05/2;
 //       ground.primitives[2].type = ground.primitives[0].BOX;
 //       ground.primitives[2].dimensions.resize(3);
 //       ground.primitives[2].dimensions[0] = 2 * x_bound;
 //       ground.primitives[2].dimensions[1] = 1 - y_bound;
 //       ground.primitives[2].dimensions[2] = .05;
 //       ground.primitive_poses[2].orientation.w = 1;
 //       ground.primitive_poses[2].position.x = 0;
 //       ground.primitive_poses[2].position.y = (1 - y_bound)/2 + y_bound;
 //       ground.primitive_poses[2].position.z = -.05/2;
 //       ground.primitives[3].type = ground.primitives[0].BOX;
 //       ground.primitives[3].dimensions.resize(3);
 //       ground.primitives[3].dimensions[0] = 2 * x_bound;
 //       ground.primitives[3].dimensions[1] = 1 - y_bound;
 //       ground.primitives[3].dimensions[2] = .05;
 //       ground.primitive_poses[3].orientation.w = 1;
 //       ground.primitive_poses[3].position.x = 0;
 //       ground.primitive_poses[3].position.y =  -((1 - y_bound)/2 + y_bound);
 //       ground.primitive_poses[3].position.z = -.05/2 - .00;

 //       ground.operation = ground.ADD;

	//colObjVec.push_back(ground);
	//colObjVec_domain_lbls.push_back("domain");

	//moveit_msgs::CollisionObject shaker;
 //       shaker.header.frame_id = "panda_link0";
 //       shaker.id = "shaker";
 //       shaker.primitives.resize(1);
 //       shaker.primitives[0].type = shaker.primitives[0].BOX;
 //       shaker.primitives[0].dimensions.resize(3);
 //       shaker.primitives[0].dimensions[0] = 0.07;
 //       shaker.primitives[0].dimensions[1] = 0.07;
 //       shaker.primitives[0].dimensions[2] = .21;
 //       shaker.primitive_poses.resize(1);
 //       shaker.primitive_poses[0].orientation.w = 1;
 //       shaker.primitive_poses[0].position.x = 0.3;
 //       shaker.primitive_poses[0].position.y = 0.0;
 //       shaker.primitive_poses[0].position.z = 0.05;
 //       shaker.operation = shaker.ADD;
	//colObjVec.push_back(shaker);
	//colObjVec_domain_lbls.push_back("domain");

	planning_scene_interface.applyCollisionObjects(colObjVec);
	//planning_scene_interface.addCollisionObjects(colObjVec);
	move_group.setEndEffectorLink("panda_link8");

	PlanningQuerySrv plan_query_srv_container(&move_group, &planning_scene_interface, &grip_client, 2, !sim_only);
	plan_query_srv_container.setWorkspace(colObjVec, colObjVec_domain_lbls);

	ros::ServiceServer plan_query_service = M_NH.advertiseService("/manipulation_planning_query", &PlanningQuerySrv::planQuery_serviceCB, &plan_query_srv_container);

	ros::waitForShutdown();
	return 0;
}

// [ INFO] [1643583882.482445135]: Recieved Planning Query
// [ INFO] [1643583882.510020483]: Received request to compute Cartesian path
// [ INFO] [1643583882.510325752]: Attempting to follow 5 waypoints for link 'panda_link8' using a step of 0.010000 m and jump threshold 0.000000 (in global reference frame)
// [ INFO] [1643583882.560499065]: Computed Cartesian path with 1 points (followed 0.000000% of requested trajectory)
// [ INFO] [1643583882.572123968]: Execution request received
// [ WARN] [1643583882.592102622]: Dropping all 1 trajectory point(s), as they occur before the current time.
// Last point is 0.000000s in the past.
// [ WARN] [1643583882.592361258]: Controller position_joint_trajectory_controller failed with error INVALID_GOAL: Dropping all 1 trajectory point(s), as they occur before the current time.
// Last point is 0.000000s in the past.
// [ WARN] [1643583882.592444904]: Controller handle position_joint_trajectory_controller reports status FAILED
// [ INFO] [1643583882.592468358]: Completed trajectory execution with status FAILED ...
// [ INFO] [1643583882.592525685]: Execution completed: FAILED
// [ INFO] [1643583882.592719400]: ABORTED: Solution found but controller failed during execution
