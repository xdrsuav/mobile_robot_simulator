#include "ros/ros.h"

#include "mobile_robot_simulator/laser_simulator.h"

namespace laser_sim {

void get_map()
{
    nav_msgs::GetMapRequest req;
    nav_msgs::GetMapResponse resp;
    if (ros::service::call("/static_map", req, resp))
    {
        map = resp.map;
        map_ptr = &map;
        ROS_INFO_STREAM("Got a " << map.info.width << "x" << map.info.height << " map with resolution " << map.info.resolution);
        have_map = true;
    }
    else 
    {
        ROS_WARN("No map received - service '/static_map' not available");
        have_map = false;
    }
    return;
    
}

void set_laser_params(std::string frame_id, float fov, unsigned int beam_count, float max_range, float min_range, float update_frequency)
{
    l_frame = frame_id;
    l_fov = fov;
    l_beams = beam_count;
    l_max_range = max_range;
    l_min_range = min_range;
    l_frequency = update_frequency;
    ROS_INFO("Updated parameters of simulated laser");
    return;
}

void get_laser_pose(tf::TransformListener * tl, double * x, double * y, double * theta)
{
    ros::Time now = ros::Time::now();
    tf::StampedTransform transf;
    try
    {
        tl->waitForTransform("/map",l_frame,now,ros::Duration(0.5));
        tl->lookupTransform("/map",l_frame,now,transf);
    }
    catch (tf::TransformException ex)
    {
        ROS_ERROR("%s",ex.what());
        throw std::runtime_error("Could not find transform");
    }
    *x = transf.getOrigin().getX();
    *y = transf.getOrigin().getY();
    *theta = tf::getYaw(transf.getRotation());
    return;
}

void update_scan(double x, double y, double theta, sensor_msgs::LaserScan * scan)
{
    // laser params
    scan->angle_min = -l_fov/2.0;
    scan->angle_max = l_fov/2.0;
    scan->angle_increment = l_fov/l_beams;
    scan->range_min = l_min_range;
    scan->range_max = l_max_range+0.001;
    std::vector<float> ranges;
    double this_range;
    double this_ang;
    // header
    scan->header.frame_id = l_frame;
    scan->header.stamp = ros::Time::now();

    for (unsigned int i=0; i<=l_beams; i++)
    {
        this_ang = theta - scan->angle_min + i*scan->angle_increment;
        this_range = find_map_range(x,y,this_ang);
        ranges.push_back(this_range);
    }
    scan->ranges = ranges;
    //calculate timing
    scan->time_increment = (1.0/l_frequency) / l_beams;
    scan->scan_time = (1.0/l_frequency);
    return;
}

double find_map_range(double x, double y, double theta)
{
    // using "A Faster Voxel Traversal Algorithm for Ray Tracing" by Amanatides & Woo
    // ======== initialization phase ======== 
    double origin[2]; // u_hat
    origin[0] = x;
    origin[1] = y;
    double dir[2]; // v_hat
    dir[0] = cos(theta);
    dir[1] = sin(theta);
    int start_x, start_y;
    get_world2map_coordinates(x,y,&start_x,&start_y);
    if (start_x<0 || start_y<0 || start_x >= map.info.width || start_y >= map.info.height)
    {
        //outside the map, find entry point
        double delta_x = abs(map.info.origin.position.x - x);
        double delta_y = abs(map.info.origin.position.y - y);
        double intersect_x = x + (dir[0] * delta_x);
        double intersect_y = y + (dir[1] * delta_y);
        get_world2map_coordinates(intersect_x,intersect_y,&start_x,&start_y);
    }
    int current[2]; // X, Y
    current[0] = start_x;
    current[1] = start_y;
   
    int step[2]; // stepX, stepY
    double tMax[2]; // tMaxX, tMaxY
    double tDelta[2]; // tDeltaX, tDeltaY
    
    double voxel_border[2];
    get_map2world_coordinates(current[0], current[1], &voxel_border[0], &voxel_border[1]);
    voxel_border[0] -= 0.5 * map.info.resolution; //this is the lower left voxel corner
    voxel_border[1] -= 0.5 * map.info.resolution; //this is the lower left voxel corner
    
    for (unsigned int i=0;i<2;i++) // for each direction (x,y)
    {
        // determine step direction
        if (dir[i] > 0.0) step[i] = -1;
        else if (dir[i] < 0.0) step[i] = 1;
        else step[i] = 0;
        
        // determine tMax, tDelta
        if (step[i] != 0) 
        {
            // corner point of voxel (in direction of ray)
            if (step[i] == 1) 
            {
                voxel_border[i] += (float) (step[i] * map.info.resolution * 1.0);
            }
            // tMax - voxel boundary crossing
            tMax[i] = (voxel_border[i] - origin[i]) / dir[i];
            // tDelta - voxel boundary distance
            tDelta[i] = map.info.resolution / fabs(dir[i]);
        } 
        else 
        {
            tMax[i] = std::numeric_limits<double>::max();
            tDelta[i] = std::numeric_limits<double>::max();
        }
        
    }
    
    //ROS_DEBUG_STREAM("Starting at index " << start_x << "," << start_y);
    
    // ======== incremental traversal ======== 
    while (true) 
    {
        // calculate range for current voxel
        unsigned int dim; // X or Y direction
        if (tMax[0] < tMax[1]) dim = 0;
        else dim = 1;
        // advance one voxel
        current[dim] += step[dim];
        tMax[dim] += tDelta[dim];
        //ROS_DEBUG_STREAM("Examining index " << current[0] << "," << current[1]);
        // are we outside the map?
        if (current[0] < 0 || current[0] >= map.info.width || current[1] < 0 || current[1] >= map.info.height)
        {
            return l_max_range;
        }
        // determine current range
        double current_range = sqrt(pow((current[0] - start_x),2) + pow((current[1] - start_y),2)) * map.info.resolution;
        // are we at max range?
        if (current_range > l_max_range) return l_max_range;
        else { 
            int occ = get_map_occupancy(current[0],current[1]);
            if (occ >= 60) // current cell is occupied
            {
                // are we below the minimum range of the laser scanner?
                if (current_range < l_min_range) continue;
                // if not, return the current range...
                return current_range;
            }
        }
    } // end while
}
    
void get_world2map_coordinates(double world_x, double world_y, int * map_x, int * map_y)
{
    *map_x = (int) (std::floor((world_x - map.info.origin.position.x) / map.info.resolution));
    *map_y = (int) (std::floor((world_y - map.info.origin.position.y) / map.info.resolution));
    ROS_INFO_STREAM_THROTTLE(1, "world2map - x: " << world_x << " map_x: " << *map_x);
    return;
}

void get_map2world_coordinates(int map_x, int map_y, double * world_x, double * world_y)
{
    *world_x = (map_x * map.info.resolution) + map.info.origin.position.x;
    *world_y = (map_y * map.info.resolution) + map.info.origin.position.y;
    ROS_INFO_STREAM_THROTTLE(1, "map2world - map_x: " << map_x << " x: " << *world_x);
    return;
}

int get_map_occupancy(int x, int y)
{
    //ROS_DEBUG_STREAM("x: " << x << " y: " << y << " index: " <<  y*map.info.width + x);
    return map.data[y*map.info.width + x];
}

} //end namespace