#pragma once

#include <sensor_msgs/PointCloud.h>
#include <ros/ros.h>
#include <eigen3/Eigen/Eigen>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include "../featureTracker/fisheye_undist.hpp"
#include <tf/transform_broadcaster.h>
#include "depth_estimator.h"

class DepthCamManager {

    ros::Publisher pub_depth_map;
    ros::Publisher pub_depth_cloud;
    ros::Publisher up_cam_info_pub, down_cam_info_pub;
    ros::Publisher pub_camera_up, pub_camera_down;
    ros::NodeHandle nh;
    tf::TransformBroadcaster br;
    bool publish_raw_image = false;

    bool estimate_front_depth = true;
    bool estimate_left_depth = false;
    bool estimate_right_depth = false;
    double downsample_ratio = 0.5;
    Eigen::Matrix3d cam_side;
    Eigen::Matrix3d cam_side_transpose;
    cv::Mat cam_side_cv, cam_side_cv_transpose;

    int pub_cloud_step = 1;
public:
    FisheyeUndist * fisheye = nullptr;

    Eigen::Quaterniond t1, t2, t3, t4, t_down, t_transpose;
    double f_side, cx_side, cy_side;

    DepthCamManager(ros::NodeHandle & _nh, FisheyeUndist * _fisheye): nh(_nh), fisheye(_fisheye) {
        pub_depth_map = nh.advertise<sensor_msgs::Image>("front_depthmap", 1);


        pub_camera_up = nh.advertise<sensor_msgs::Image>("/front_stereo/left/image_raw", 1);
        pub_camera_down = nh.advertise<sensor_msgs::Image>("/front_stereo/right/image_raw", 1);

        up_cam_info_pub = nh.advertise<sensor_msgs::CameraInfo>("/front_stereo/left/camera_info", 1);
        down_cam_info_pub = nh.advertise<sensor_msgs::CameraInfo>("/front_stereo/right/camera_info", 1);

        pub_depth_cloud = nh.advertise<sensor_msgs::PointCloud>("depth_cloud", 1000);
        t1 = Eigen::Quaterniond(Eigen::AngleAxisd(-M_PI / 2, Eigen::Vector3d(1, 0, 0)));
        t2 = t1 * Eigen::AngleAxisd(M_PI / 2, Eigen::Vector3d(0, 1, 0));
        t3 = t2 * Eigen::AngleAxisd(M_PI / 2, Eigen::Vector3d(0, 1, 0));
        t4 = t3 * Eigen::AngleAxisd(M_PI / 2, Eigen::Vector3d(0, 1, 0));
        t_down = Eigen::Quaterniond(Eigen::AngleAxisd(M_PI, Eigen::Vector3d(1, 0, 0)));

        t_transpose = Eigen::AngleAxisd(M_PI / 2, Eigen::Vector3d(0, 0, 1));

        f_side = fisheye->f_side;
        cx_side = fisheye->cx_side;
        cy_side = fisheye->cy_side;
        cam_side << f_side*downsample_ratio, 0, cx_side*downsample_ratio,
                     0, f_side*downsample_ratio, cy_side*downsample_ratio, 0, 0, 1;
        cam_side_transpose << f_side*downsample_ratio, 0, cy_side*downsample_ratio,
                0, f_side*downsample_ratio, cx_side*downsample_ratio, 0, 0, 1;
        cv::eigen2cv(cam_side, cam_side_cv);
        cv::eigen2cv(cam_side_transpose, cam_side_cv_transpose);
    }

    void update_images(ros::Time stamp, std::vector<cv::cuda::GpuMat> & up_cams, std::vector<cv::cuda::GpuMat> & down_cams,
        Eigen::Matrix3d ric1, Eigen::Vector3d tic1,
        Eigen::Matrix3d ric2, Eigen::Vector3d tic2, 
        Eigen::Matrix3d R, Eigen::Vector3d P
    ) {
        
        if (estimate_front_depth) {
            cv::cuda::GpuMat up_front, down_front;
            ric1 = ric1*t2*t_transpose;
            ric2 = ric2*t_down*t2*t_transpose;
            cv::cuda::resize(up_cams[2], up_front, cv::Size(), downsample_ratio, downsample_ratio);
            cv::cuda::resize(down_cams[2], down_front, cv::Size(), downsample_ratio, downsample_ratio);
            if (up_cams[2].channels() == 3) {
                cv::cuda::cvtColor(up_front, up_front, cv::COLOR_BGR2GRAY);
                cv::cuda::cvtColor(down_front, down_front, cv::COLOR_BGR2GRAY);
            }

            cv::Size target(up_front.size().height, up_front.size().width);
            // cv::cuda::rotate(up_front, up_front,    target, -90);
            // cv::cuda::rotate(down_front, down_front, target, -90);

            //After transpose, we need flip for rotation

            cv::cuda::GpuMat tmp;

            cv::cuda::transpose(up_front, tmp);
            cv::cuda::flip(tmp, up_front, 0);

            cv::cuda::transpose(down_front, tmp);
            cv::cuda::flip(tmp, down_front, 0);

            Eigen::Vector3d t01 = tic2 - tic1;
            std::cout << "T01" << t01 << std::endl;
            // t01.x() = t01.x()*fisheye->f_side*downsample_ratio;
            // t01.y() = 0;
            // t01.z() = 0;
            t01 = ric1.transpose()*t01;

            // std::cout << tic <<std::endl;
            std::cout << "R" << ric1.transpose() * ric2 << "\nT" << t01 << std::endl;
            DepthEstimator dep(-t01, ric1.transpose() * ric2, cam_side_cv_transpose, SHOW_TRACK);
            cv::Mat pointcloud_up = dep.ComputeDepthCloud(up_front, down_front);

            cv::Mat up_front_cpu;
            cv::Mat tmp2;
            up_cams[2].download(up_front_cpu);
            cv::transpose(up_front_cpu, tmp2);
            cv::flip(tmp2, up_front_cpu, 0);
            cv::resize(up_front_cpu, up_front_cpu, cv::Size(), downsample_ratio, downsample_ratio);
            if (pub_cloud_step > 0) { 
                // publish_world_point_cloud(pointcloud_up, R*ric1, P+tic1, stamp, 3, up_front_cpu);
                publish_world_point_cloud(pointcloud_up, R*ric1, P+R*tic1, stamp, pub_cloud_step, up_front_cpu);
            }
        }
    
        if (publish_raw_image) {
            publish_front_images_for_external_sbgm(stamp, up_cams[2], down_cams[2], ric1, tic1, ric2, tic2, R, P);
        }

    }

    void publish_world_point_cloud(cv::Mat pts3d, Eigen::Matrix3d R, Eigen::Vector3d P, ros::Time stamp, int step = 3, cv::Mat color = cv::Mat()) {
        std::cout<< "Pts3d Size " << pts3d.size() << std::endl;
        std::cout<< "Color Size " << color.size() << std::endl;
        sensor_msgs::PointCloud point_cloud;
        point_cloud.header.stamp = stamp;
        point_cloud.header.frame_id = "world";
        point_cloud.channels.resize(3);
        point_cloud.channels[0].name = "rgb";
        point_cloud.channels[0].values.resize(0);
        point_cloud.channels[1].name = "u";
        point_cloud.channels[1].values.resize(0);
        point_cloud.channels[2].name = "v";
        point_cloud.channels[2].values.resize(0);
  
        for(int v = 0; v < pts3d.rows; v += step){
            for(int u = 0; u < pts3d.cols; u += step)  
            {
                cv::Vec3f vec = pts3d.at<cv::Vec3f>(v, u);
                double x = vec[0];
                double y = vec[1];
                double z = vec[2];
                if (z > 0.2) {
                    Vector3d pts_i(x, y, z);
                    Vector3d w_pts_i = R * pts_i + P;
                    // Vector3d w_pts_i = pts_i;

                    geometry_msgs::Point32 p;
                    p.x = w_pts_i(0);
                    p.y = w_pts_i(1);
                    p.z = w_pts_i(2);
                    
                    point_cloud.points.push_back(p);

                    const cv::Vec3b& bgr = color.at<cv::Vec3b>(v, u);
                    int32_t rgb_packed = (bgr[2] << 16) | (bgr[1] << 8) | bgr[0];
                    point_cloud.channels[0].values.push_back(*(float*)(&rgb_packed));

                    point_cloud.channels[1].values.push_back(u);
                    point_cloud.channels[2].values.push_back(v);
                }
            }
        }
        pub_depth_cloud.publish(point_cloud);
    }

    void publish_front_images_for_external_sbgm(ros::Time stamp, const cv::cuda::GpuMat front_up, const cv::cuda::GpuMat front_down,
            Eigen::Matrix3d ric1, Eigen::Vector3d tic1,
            Eigen::Matrix3d ric2, Eigen::Vector3d tic2, 
            Eigen::Matrix3d R, Eigen::Vector3d P) {

        sensor_msgs::CameraInfo cam_info_left, cam_info_right;
        cam_info_left.K[0] = fisheye->f_side*downsample_ratio;
        cam_info_left.K[1] = 0;
        // cam_info_left.K[2] = fisheye->cx_side;
        cam_info_left.K[2] = fisheye->cy_side*downsample_ratio;
        cam_info_left.K[3] = 0;
        cam_info_left.K[4] = fisheye->f_side;
        // cam_info_left.K[5] = fisheye->cy_side;
        cam_info_left.K[5] = fisheye->cx_side*downsample_ratio;
        cam_info_left.K[6] = 0;
        cam_info_left.K[7] = 0;
        cam_info_left.K[8] = 1;

        cam_info_left.header.stamp = stamp;
        cam_info_left.header.frame_id = "camera_up_front";
        // cam_info_left.width = fisheye->imgWidth;
        // cam_info_left.height = fisheye->sideImgHeight;
        cam_info_left.width = fisheye->sideImgHeight*downsample_ratio;
        cam_info_left.height = fisheye->imgWidth*downsample_ratio;
        // cam_info_left.
        // cam_info_left.
        cam_info_left.P[0] = fisheye->f_side*downsample_ratio;
        cam_info_left.P[1] = 0;
        cam_info_left.P[2] = cam_info_left.K[2];
        cam_info_left.P[3] = 0;

        cam_info_left.P[4] = 0;
        cam_info_left.P[5] = fisheye->f_side*downsample_ratio;
        cam_info_left.P[6] = cam_info_left.K[5];
        cam_info_left.P[7] = 0;
        
        cam_info_left.P[8] = 0;
        cam_info_left.P[9] = 0;
        cam_info_left.P[10] = 1;
        cam_info_left.P[11] = 0;

        // ric1 = Eigen::Matrix3d::Identity();
        // ric2 = Eigen::Matrix3d::Identity();
        ric1 = ric1*t2*t_transpose;
        ric2 = ric2*t_down*t2*t_transpose;
        Eigen::Vector3d t01 = tic2 - tic1;
        // t01 = R0.transpose() * t01;
        t01 = ric1.transpose() * t01;
        
        Eigen::Matrix3d R01 = (ric1.transpose() * ric2);

        cam_info_right = cam_info_left;
        cam_info_right.P[3] = -t01.x()*fisheye->f_side*downsample_ratio;
        cam_info_right.P[7] = 0;
        cam_info_right.P[11] = 0;

        Eigen::Matrix3d R_iden = Eigen::Matrix3d::Identity();
        memcpy(cam_info_left.R.data(), R_iden.data(), 9*sizeof(double));
        memcpy(cam_info_right.R.data(), R01.data(), 9*sizeof(double));


        up_cam_info_pub.publish(cam_info_left);
        down_cam_info_pub.publish(cam_info_right);

        cv::Mat up_cam, down_cam;
        front_up.download(up_cam);
        front_down.download(down_cam);

        cv::transpose(up_cam, up_cam);
        cv::transpose(down_cam, down_cam);

        cv::resize(up_cam, up_cam, cv::Size(), 0.5, 0.5);
        cv::resize(down_cam, down_cam, cv::Size(), 0.5, 0.5);
        sensor_msgs::ImagePtr up_img_msg = cv_bridge::CvImage(std_msgs::Header(), "bgr8", up_cam).toImageMsg();
        up_img_msg->header.stamp = stamp;
        up_img_msg->header.frame_id = "camera_up_front";
        sensor_msgs::ImagePtr down_img_msg = cv_bridge::CvImage(std_msgs::Header(), "bgr8", down_cam).toImageMsg();
        down_img_msg->header.stamp = stamp;
        down_img_msg->header.frame_id = "camera_up_front";
        

        pub_camera_up.publish(up_img_msg);
        pub_camera_down.publish(down_img_msg);

        Eigen::Vector3d cam_pos = tic1 + P;
        Eigen::Quaterniond cam_quat(ric1);
        tf::Transform transform;
        transform.setOrigin( tf::Vector3(cam_pos.x(), cam_pos.y(), cam_pos.z()) );
        tf::Quaternion q(cam_quat.x(), cam_quat.y(), cam_quat.z(), cam_quat.w());
        transform.setRotation(q);
        br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "world", "camera_up_front"));
    }

    void publish_depthmap(cv::Mat & depthmap) {
        sensor_msgs::ImagePtr depth_img_msg = cv_bridge::CvImage(std_msgs::Header(), "32FC1", depthmap).toImageMsg();
        pub_depth_map.publish(depth_img_msg);
    }

    
};