/*
 * This is part of the Bayesian Object Tracking (bot),
 * (https://github.com/bayesian-object-tracking)
 *
 * Copyright (c) 2015 Max Planck Society,
 * 				 Autonomous Motion Department,
 * 			     Institute for Intelligent Systems
 *
 * This Source Code Form is subject to the terms of the GNU General Public
 * License License (GNU GPL). A copy of the license can be found in the LICENSE
 * file distributed with this source code.
 */

/**
 * \file  object_tracker_controller_service_node.cpp
 * \date April 2016
 * \author Jan Issac (jan.issac@gmail.com)
 */

#include <Eigen/Dense>

#include <fstream>
#include <ctime>
#include <memory>
#include <thread>

#include <ros/ros.h>
#include <ros/package.h>

#include <fl/util/profiling.hpp>

#include <opi/interactive_marker_initializer.hpp>
#include <osr/free_floating_rigid_bodies_state.hpp>

#include <dbot/util/camera_data.hpp>
#include <dbot/util/simple_wavefront_object_loader.hpp>
#include <dbot/tracker/rbc_particle_filter_object_tracker.hpp>
#include <dbot/tracker/builder/rbc_particle_filter_tracker_builder.hpp>

#include <dbot_ros/tracker_node.h>
#include <dbot_ros/object_tracker_publisher.h>
#include <dbot_ros/utils/ros_interface.hpp>
#include <dbot_ros/utils/ros_camera_data_provider.hpp>

#include <dbot_ros_msgs/TrackObject.h>
#include <dbot_ros_msgs/FindObject.h>
#include <dbot_ros_msgs/RunObjectTracker.h>

static bool running = false;
static std::thread tracker_thread;
static std::shared_ptr<opi::InteractiveMarkerInitializer> object_initializer;

static ros::ServiceClient object_finder_service_client;
static ros::ServiceClient object_tracker_service_client;

bool stop_object_tracker()
{
    ros::NodeHandle nh_prv("~");
    std::string objects_package;
    std::string objects_directory;
    nh_prv.getParam("objects/package", objects_package);
    nh_prv.getParam("objects/directory", objects_directory);

    dbot_ros_msgs::RunObjectTracker run_object_tracker_srv;
    run_object_tracker_srv.request.object_state.ori.package = objects_package;
    run_object_tracker_srv.request.object_state.ori.directory =
        objects_directory;
    run_object_tracker_srv.request.object_state.ori.name = "stop";

    ROS_INFO("Stopping object tracker ...");
    if (!object_tracker_service_client.call(run_object_tracker_srv))
    {
        ROS_ERROR("Stopping object tracker failed.");
        return false;
    }
    return true;
}

bool track_object_srv(dbot_ros_msgs::TrackObjectRequest& req,
                      dbot_ros_msgs::TrackObjectResponse& res)
{
    if (req.object_name == "stop")
    {
        return stop_object_tracker();
    }

    // stop tracker first
    if (!stop_object_tracker())
    {
        return false;
    }

    // find new pose
    ros::NodeHandle nh_prv("~");
    std::string objects_package;
    std::string objects_directory;
    nh_prv.getParam("objects/package", objects_package);
    nh_prv.getParam("objects/directory", objects_directory);

    geometry_msgs::PoseStamped pose;

    if (req.auto_detect)
    {
        dbot_ros_msgs::FindObject find_object_srv;
        find_object_srv.request.object_ori.package = objects_package;
        find_object_srv.request.object_ori.directory = objects_directory;
        find_object_srv.request.object_ori.name = req.object_name + ".obj";
        if (!object_finder_service_client.call(find_object_srv))
        {
            ROS_ERROR("Finding object '%s' failed.", req.object_name.c_str());
            return false;
        }
        ROS_INFO_STREAM("Object found:\n" << find_object_srv.response);

        if (!req.auto_confirm)
        {
            object_initializer->set_object(
                find_object_srv.response.found_object.ori.package,
                find_object_srv.response.found_object.ori.directory,
                find_object_srv.response.found_object.ori.name,
                find_object_srv.response.found_object.pose.pose,
                false);
        }

        pose = find_object_srv.response.found_object.pose;
    }
    else
    {
        object_initializer->set_objects(objects_package,
                                        objects_directory,
                                        {req.object_name + ".obj"},
                                        {},
                                        true,
                                        !req.auto_confirm);

        pose.pose = object_initializer->poses()[0];
    }

    if (!req.auto_confirm)
    {
        ROS_INFO(
            "Object pose set. Confirm the pose by clicking on the "
            "interactive marker!");

        if (!object_initializer->wait_for_object_poses())
        {
            ROS_INFO("Setting object poses was interrupted.");
            return false;
        }

        ROS_INFO("Object pose confirmed. Triggering object tracker ...");

        pose.pose = object_initializer->poses()[0];
    }

    // trigger tracking
    dbot_ros_msgs::RunObjectTracker run_object_tracker_srv;
    run_object_tracker_srv.request.object_state.ori.package = objects_package;
    run_object_tracker_srv.request.object_state.ori.directory =
        objects_directory;
    run_object_tracker_srv.request.object_state.ori.name =
        req.object_name + ".obj";
    run_object_tracker_srv.request.object_state.pose = pose;
    if (!object_tracker_service_client.call(run_object_tracker_srv))
    {
        ROS_ERROR("Running object tracker for '%s' failed.",
                  req.object_name.c_str());
        return false;
    }

    return true;
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "object_tracker_controller_service");
    ros::NodeHandle nh;
    ros::NodeHandle nh_prv("~");

    std::string object_tracker_controller_service_name;
    std::string object_tracker_service_name;
    nh_prv.getParam("object_tracker_controller_service_name",
                    object_tracker_controller_service_name);
    nh_prv.getParam("object_tracker_service_name", object_tracker_service_name);

    auto camera_frame =
        ri::get_camera_frame("/XTION/depth/camera_info", nh, 5.);

    if (camera_frame.empty())
    {
        ROS_ERROR("Cannot obtain camera frame.");
        return 1;
    }

    object_initializer =
        std::make_shared<opi::InteractiveMarkerInitializer>(camera_frame);

    auto srv = nh.advertiseService("object_tracker_controller_service",
                                   track_object_srv);

    object_finder_service_client =
        nh.serviceClient<dbot_ros_msgs::FindObject>("object_finder_service");
    object_finder_service_client.waitForExistence();

    object_tracker_service_client =
        nh.serviceClient<dbot_ros_msgs::RunObjectTracker>(
            "object_tracker_service");
    object_tracker_service_client.waitForExistence();

    ROS_INFO("Object tracker controller service up and running.");
    ROS_INFO("Waiting for requests...");

    ros::spin();

    return 0;
}
