#include <ndt_registration/ndt_matcher_d2d_sc.h>
#include <ndt_registration/icp_matcher_p2p.h>
#include <ndt_generic/motion_model_3d.h>
#include <ndt_generic/io.h>
#include <ndt_map/pointcloud_utils.h>
#include <boost/program_options.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/foreach.hpp>
#include "gnuplot-iostream.h"

using namespace std;


class NDTD2DSCScore 
{
public:
    Eigen::Affine3d offset;
    double score_d2d;
    double score_d2d_sc;
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};

// To evaluate the objective function with and without soft constraints, to interface the files generated by the offline fuser
class NDTD2DSCScoreEval 
{
public:
    
    NDTD2DSCScoreEval(const std::string &gt_file,
                      const std::string &odom_file,
                      const std::string &base_name_pcd,
                      const Eigen::Affine3d &sensor_pose,
                      const lslgeneric::MotionModel3d::Params &motion_params) : base_name_pcd_(base_name_pcd), sensor_pose_(sensor_pose) {
        
        
        std::cout << "loading: " << gt_file << std::endl;
        Tgt = ndt_generic::loadAffineFromEvalFile(gt_file);
        std::cout << " got # poses : " << Tgt.size() << std::endl;
        std::cout << "loading: " << odom_file << std::endl;
        Todom = ndt_generic::loadAffineFromEvalFile(odom_file);
        std::cout << " got #poses : " << Todom.size() << std::endl;
        
        motion_model.setParams(motion_params);
        resolution = 1.;
        alpha_ = 1.;

        offset_size = 100;
        incr_dist = 0.01;
        incr_ang = 0.002;

        T_glb_d2d_.setIdentity();
        T_glb_d2d_sc_.setIdentity();
        T_glb_icp_.setIdentity();
        T_glb_filter_.setIdentity();
        T_glb_icp_filter_.setIdentity();
        T_glb_gt_.setIdentity();
        T_glb_odom_.setIdentity();
    }
    
    std::vector<Eigen::Affine3d> generateOffsetSet() {
        
        std::vector<Eigen::Affine3d> ret;
        std::vector<double> incr(6);
        incr.resize(6);

        for (int i = 0; i < 3; i++) {
            incr[i] = incr_dist;
            incr[i+3] = incr_ang;
        }
        
        Eigen::VectorXd offset(6);
        for (int j = 0; j < 6; j++) {
            offset.setZero();
            for (int i = -offset_size; i <= offset_size; i++) {
                offset[j] = i*incr[j];
                ret.push_back(ndt_generic::vectorToAffine3d(offset));
            }
            
        }
        return ret;
    }
    
    std::vector<Eigen::Affine3d> generateOffset2DSet(int dimidx1, int dimidx2) {
        
        std::vector<Eigen::Affine3d> ret;
        std::vector<double> incr(6);
        incr.resize(6);

        for (int i = 0; i < 3; i++) {
            incr[i] = incr_dist;
            incr[i+3] = incr_ang;
        }
        
        Eigen::VectorXd offset(6);
        offset.setZero();
        for (int j = -offset_size ; j <= offset_size; j++) {
            offset[dimidx1] = j*incr[dimidx1];
            for (int i = -offset_size; i <= offset_size; i++) {
                offset[dimidx2] = i*incr[dimidx2];
                ret.push_back(ndt_generic::vectorToAffine3d(offset));
            }
        }

        return ret;
    }
    
    int nbPoses() const {
        if (Todom.size() < Tgt.size())
            return Todom.size();
        return Tgt.size();
    }

    void computeScoreSets(int idx1, int idx2, int dimidx1, int dimidx2) {
        
        // The pcd is given in sensor frame...
        pcl::PointCloud<pcl::PointXYZ> pc1, pc2;
        ndt_generic::loadCloud(base_name_pcd_, idx1, pc1);
        ndt_generic::loadCloud(base_name_pcd_, idx2, pc2);
        
        if (pc1.empty() || pc2.empty()) {
            std::cerr << "no points(!) found" << std::endl;
            return;
        }
        
        std::cout << "loaded pc1 # points : " << pc1.size() << std::endl;
        std::cout << "loaded pc2 # points : " << pc2.size() << std::endl;
        std::cout << "Sensor pose used : " << ndt_generic::affine3dToStringRotMat(sensor_pose_);

        // Work in the vehicle frame... move the points
        lslgeneric::transformPointCloudInPlace(sensor_pose_, pc1);
        lslgeneric::transformPointCloudInPlace(sensor_pose_, pc2);

        // Compute the ndtmap
        lslgeneric::NDTMap nd1(new lslgeneric::LazyGrid(resolution), true);
        nd1.loadPointCloud(pc1);
        nd1.computeNDTCellsSimple();
        
        lslgeneric::NDTMap nd2(new lslgeneric::LazyGrid(resolution), true);
        nd2.loadPointCloud(pc2);
        nd2.computeNDTCellsSimple();
        
        // Relative odometry
        T_rel_odom_ = Todom[idx1].inverse() * Todom[idx2];
        
        std::cout << "T_rel_odom : " << ndt_generic::affine3dToStringRPY(T_rel_odom_) << std::endl;

        // Compute the covariance of the motion
        Eigen::MatrixXd Tcov = motion_model.getCovMatrix(T_rel_odom_); 
       
        lslgeneric::NDTMatcherD2DSC matcher_d2d_sc;
        
        std::vector<Eigen::Affine3d> Ts = generateOffset2DSet(dimidx1,dimidx2);
        results.resize(Ts.size());
        
        for (int i = 0; i < Ts.size(); i++) {
            double score_d2d, score_d2d_sc;
            std::cout << "." << std::flush;
            matcher_d2d_sc.scoreComparision(nd1, nd2, T_rel_odom_, Tcov, score_d2d, score_d2d_sc, Ts[i], alpha_);
            results[i].offset = Ts[i];
            results[i].score_d2d = score_d2d;
            results[i].score_d2d_sc = score_d2d_sc;
        }
        
        // Compute matches
        lslgeneric::NDTMatcherD2D matcher_d2d;
        T_d2d_ = T_rel_odom_;
        T_d2d_sc_ = T_rel_odom_;
        matcher_d2d.match(nd1, nd2, T_d2d_, true);
        matcher_d2d_sc.match(nd1, nd2, T_d2d_sc_, Tcov);
      

        // Compute a "filtered" match - based on the d2d output and odometry weighed by the covariances.
        Eigen::MatrixXd T_d2d_cov(6,6);
        if (matcher_d2d.covariance(nd1, nd2, T_d2d_, T_d2d_cov)) {
            std::cout << "T_d2d_cov : " << T_d2d_cov << std::endl;
            std::cout << "T_d2d_cov condition : " << ndt_generic::getCondition(T_d2d_cov) << std::endl;
            std::cout << "T_d2d_cov.inverse() : " << T_d2d_cov.inverse() << std::endl;
            T_filter_ = ndt_generic::getWeightedPose(T_rel_odom_, Tcov, T_d2d_, T_d2d_cov);
        }
        else {
            T_filter_ = T_d2d_;
        }

        // Compute matches using ICP along with the cov
        ICPMatcherP2P matcher_icp;
        matcher_icp.match(pc1, pc2, T_icp_);
        pc_icp_final_ = matcher_icp.getFinalPC();
        Eigen::MatrixXd T_icp_cov(6,6);
        if (matcher_icp.covariance(pc1, pc2, T_icp_, T_icp_cov)) {
            std::cout << "T_icp_cov : " << T_icp_cov << std::endl;
            std::cout << "T_icp_cov condition : " << ndt_generic::getCondition(T_icp_cov) << std::endl;
            std::cout << "T_icp_cov.inverse() : " << T_icp_cov.inverse() << std::endl;
            
            T_icp_filter_ = ndt_generic::getWeightedPose(T_rel_odom_, Tcov, T_icp_, T_icp_cov);
        }
        else {
            // Should not happen
            T_icp_filter_ = T_icp_;
        }


        // Update the relative transform.
        T_rel_gt_ = Tgt[idx1].inverse() * Tgt[idx2];

        // Store the transformed pcd
        pc_odom_ = lslgeneric::transformPointCloud(T_rel_odom_, pc2);
        pc_gt_ = lslgeneric::transformPointCloud(T_rel_gt_, pc2);
        pc_d2d_ = lslgeneric::transformPointCloud(T_d2d_, pc2);
        pc_d2d_sc_ = lslgeneric::transformPointCloud(T_d2d_sc_, pc2);
        pc_icp_ = lslgeneric::transformPointCloud(T_icp_, pc2);
        pc1_ = pc1;

        // Update the global transf (using odom and gt as well here incase the idx changes)
        T_glb_d2d_ = T_glb_d2d_ * T_d2d_;
        T_glb_d2d_sc_ = T_glb_d2d_sc_ * T_d2d_sc_;
        T_glb_icp_ = T_glb_icp_ * T_icp_;
        T_glb_filter_ = T_glb_filter_ * T_filter_;
        T_glb_icp_filter_ = T_glb_icp_filter_ * T_icp_filter_;
        T_glb_gt_ = T_glb_gt_ * T_rel_gt_;
        T_glb_odom_ = T_glb_odom_ * T_rel_odom_;

        Ts_glb_d2d_.push_back(T_glb_d2d_);
        Ts_glb_d2d_sc_.push_back(T_glb_d2d_sc_);
        Ts_glb_icp_.push_back(T_glb_icp_);
        Ts_glb_filter_.push_back(T_glb_filter_);
        Ts_glb_icp_filter_.push_back(T_glb_icp_filter_);
        Ts_glb_gt_.push_back(T_glb_gt_);
        Ts_glb_odom_.push_back(T_glb_odom_);
    }

    void save(const std::string &filename, int grididx) const {

        std::cout << "saving to : " << filename << std::endl;
        // Save the output for plotting...
        std::ofstream ofs;
        ofs.open(filename.c_str());
        if (!ofs.is_open())
            return;
        
        for (int i = 0; i < results.size(); i++) {
            ofs << ndt_generic::affine3dToStringRPY(results[i].offset) << " " << results[i].score_d2d << " " << results[i].score_d2d_sc << std::endl;

            // for gnuplot - if you want to plot a grid you need to throw in some blank lines
            if (i < results.size()-1) {
                if (results[i].offset.translation()[grididx] !=
                    results[i+1].offset.translation()[grididx]) {
                    ofs << std::endl;
                }
            }
        }
        ofs.close();
    }

    // Save in a 2d grid, to be able to plot a 2d grid with different colors (gnuplot pm3d)
    void save2D(const std::string &filename, int dimidx1, int dimidx2) const {

        std::cout << "saving to : " << filename << std::endl;
        // Save the output for plotting...
        std::ofstream ofs;
        ofs.open(filename.c_str());
        if (!ofs.is_open())
            return;
        
        for (int i = 0; i < results.size(); i++) {
            Eigen::VectorXd x = ndt_generic::affine3dToVector(results[i].offset);
            ofs << x[dimidx1] << " " << x[dimidx2] << " " << results[i].score_d2d << " " << results[i].score_d2d_sc << std::endl;

            // for gnuplot - if you want to plot a grid you need to throw in some blank lines
            if (i < results.size()-1) {
                if (results[i].offset.translation()[dimidx1] !=
                    results[i+1].offset.translation()[dimidx1]) {
                    ofs << std::endl;
                }
            }
        }
        ofs.close();

    }
    
    void savePCD(const std::string &filename) const {
        pcl::io::savePCDFile<pcl::PointXYZ>(filename + ".gt.pcd", pc_gt_);
        pcl::io::savePCDFile<pcl::PointXYZ>(filename + ".odom.pcd", pc_odom_);
        pcl::io::savePCDFile<pcl::PointXYZ>(filename + ".d2d.pcd", pc_d2d_);
        pcl::io::savePCDFile<pcl::PointXYZ>(filename + ".d2d_sc.pcd", pc_d2d_sc_);
        pcl::io::savePCDFile<pcl::PointXYZ>(filename + ".icp.pcd", pc_icp_);
        pcl::io::savePCDFile<pcl::PointXYZ>(filename + ".icp_final.pcd", pc_icp_final_);
        pcl::io::savePCDFile<pcl::PointXYZ>(filename + ".pc1.pcd", pc1_);
    }

    
    // Saves in the same frame as with the offset is used (the odometry frame)
    void savePoseEstInOdomFrame(const std::string &filename) const {
        ndt_generic::saveAffine3dRPY(filename + std::string(".gt"), T_rel_odom_.inverse()*T_rel_gt_);
        ndt_generic::saveAffine3dRPY(filename + std::string(".d2d"), T_rel_odom_.inverse()*T_d2d_);
        ndt_generic::saveAffine3dRPY(filename + std::string(".d2d_sc"), T_rel_odom_.inverse()*T_d2d_sc_);
        ndt_generic::saveAffine3dRPY(filename + std::string(".icp"), T_rel_odom_.inverse()*T_icp_);
        ndt_generic::saveAffine3dRPY(filename + std::string(".filter"), T_rel_odom_.inverse()*T_filter_);
        ndt_generic::saveAffine3dRPY(filename + std::string(".odom"), T_rel_odom_.inverse()*T_rel_odom_);
    }

    void saveTsToEvalFiles(const std::string &filename) const {
        ndt_generic::saveAffineToEvalFile(filename + std::string(".gt"), Ts_glb_gt_);
        ndt_generic::saveAffineToEvalFile(filename + std::string(".d2d"), Ts_glb_d2d_);
        ndt_generic::saveAffineToEvalFile(filename + std::string(".d2d_sc"), Ts_glb_d2d_sc_);
        ndt_generic::saveAffineToEvalFile(filename + std::string(".icp"), Ts_glb_icp_);
        ndt_generic::saveAffineToEvalFile(filename + std::string(".filter"), Ts_glb_filter_);
        ndt_generic::saveAffineToEvalFile(filename + std::string(".icp_filter"), Ts_glb_icp_);
        ndt_generic::saveAffineToEvalFile(filename + std::string(".odom"), Ts_glb_odom_);
    }

    std::vector<std::vector<boost::tuple<double, double, double> > > getScoreSegments(int dimidx1, int dimidx2, bool d2d_sc_score) const {
        
        std::vector<std::vector<boost::tuple<double, double, double> > > ret;
        std::vector<boost::tuple<double, double, double> > segm;
        for (int i = 0; i < results.size(); i++) {
            Eigen::VectorXd x = ndt_generic::affine3dToVector(results[i].offset);
            double score = results[i].score_d2d;
            if (d2d_sc_score) {
                score = results[i].score_d2d_sc;
            }
            segm.push_back(boost::make_tuple(x[dimidx1], x[dimidx2], score));
            
            if (i < results.size()-1) {
                if (results[i].offset.translation()[dimidx1] !=
                    results[i+1].offset.translation()[dimidx1]) {
                    ret.push_back(segm);
                    segm.resize(0);
                }
            }
            
	}
	return ret;
    }

    std::vector<std::vector<boost::tuple<double, double, double> > > getRelPoseGT(int dimidx1, int dimidx2) const {
        
        std::vector<std::vector<boost::tuple<double, double, double> > > ret;
        std::vector<boost::tuple<double, double, double> > segm;
        Eigen::VectorXd t = ndt_generic::affine3dToVector(T_rel_odom_.inverse() * T_rel_gt_);
        segm.push_back(boost::tuple<double,double,double>(t[dimidx1],t[dimidx2], 0.));
        ret.push_back(segm);
        return ret;
    }

    std::vector<boost::tuple<double, double, double> > getRelPoseD2D(int dimidx1, int dimidx2) const {
        
        std::vector<boost::tuple<double, double, double> > ret;
        Eigen::VectorXd t = ndt_generic::affine3dToVector(T_rel_odom_.inverse() * T_d2d_);
        ret.push_back(boost::tuple<double,double,double>(t[dimidx1],t[dimidx2], 0.));
        return ret;
    }

    std::vector<boost::tuple<double, double, double> > getRelPoseD2D_SC(int dimidx1, int dimidx2) const {
        
        std::vector<boost::tuple<double, double, double> > ret;
        Eigen::VectorXd t = ndt_generic::affine3dToVector(T_rel_odom_.inverse() * T_d2d_sc_);
        ret.push_back(boost::tuple<double,double,double>(t[dimidx1],t[dimidx2], 0.));
        return ret;
    }

    std::vector<boost::tuple<double, double, double> > getRelPoseICP(int dimidx1, int dimidx2) const {
        
        std::vector<boost::tuple<double, double, double> > ret;
        Eigen::VectorXd t = ndt_generic::affine3dToVector(T_rel_odom_.inverse() * T_icp_);
        ret.push_back(boost::tuple<double,double,double>(t[dimidx1],t[dimidx2], 0.));
        return ret;
    }

    std::vector<boost::tuple<double, double, double> > getRelPoseFilter(int dimidx1, int dimidx2) const {
        
        std::vector<boost::tuple<double, double, double> > ret;
        Eigen::VectorXd t = ndt_generic::affine3dToVector(T_rel_odom_.inverse() * T_filter_);
        ret.push_back(boost::tuple<double,double,double>(t[dimidx1],t[dimidx2], 0.));
        return ret;
    }

    std::vector<boost::tuple<double, double, double> > getRelPoseICPFilter(int dimidx1, int dimidx2) const {
        
        std::vector<boost::tuple<double, double, double> > ret;
        Eigen::VectorXd t = ndt_generic::affine3dToVector(T_rel_odom_.inverse() * T_icp_filter_);
        ret.push_back(boost::tuple<double,double,double>(t[dimidx1],t[dimidx2], 0.));
        return ret;
    }

    std::vector<std::pair<double, double> > getRelPoseOdom(int dimidx1, int dimidx2) const {
        // Always 0,0,0,0,0,0....
        std::vector<std::pair<double, double> > ret;
        Eigen::VectorXd t = ndt_generic::affine3dToVector(T_rel_odom_.inverse() * T_rel_odom_);
        ret.push_back(std::pair<double,double>(t[dimidx1],t[dimidx2]));
        return ret;
    }


    
    // Parameters
    double resolution; // resolution for the ndt map

    int offset_size;
    double incr_dist;
    double incr_ang;
    
private:
    std::vector<Eigen::Affine3d> Todom;
    std::vector<Eigen::Affine3d> Tgt;

    std::string base_name_pcd_;
    Eigen::Affine3d sensor_pose_;

    lslgeneric::MotionModel3d motion_model;
    
    
    std::vector<NDTD2DSCScore> results;
    double alpha_;

    Eigen::Affine3d T_d2d_;
    Eigen::Affine3d T_d2d_sc_;
    Eigen::Affine3d T_icp_;
    Eigen::Affine3d T_filter_;
    Eigen::Affine3d T_icp_filter_;
    Eigen::Affine3d T_rel_gt_;
    Eigen::Affine3d T_rel_odom_;

    Eigen::Affine3d T_glb_d2d_;
    Eigen::Affine3d T_glb_d2d_sc_;
    Eigen::Affine3d T_glb_icp_;
    Eigen::Affine3d T_glb_filter_;
    Eigen::Affine3d T_glb_icp_filter_;
    Eigen::Affine3d T_glb_gt_;
    Eigen::Affine3d T_glb_odom_;

    std::vector<Eigen::Affine3d> Ts_glb_d2d_;
    std::vector<Eigen::Affine3d> Ts_glb_d2d_sc_;
    std::vector<Eigen::Affine3d> Ts_glb_icp_;
    std::vector<Eigen::Affine3d> Ts_glb_filter_;
    std::vector<Eigen::Affine3d> Ts_glb_icp_filter_;
    std::vector<Eigen::Affine3d> Ts_glb_gt_;
    std::vector<Eigen::Affine3d> Ts_glb_odom_;

    pcl::PointCloud<pcl::PointXYZ> pc_odom_, pc_d2d_, pc_d2d_sc_, pc_icp_, pc_gt_, pc1_, pc_icp_final_;
};


namespace po = boost::program_options;

int main(int argc, char** argv)
{
    cout << "--------------------------------------------------" << endl;
    cout << "Creates a set of gnuplot files to visualize " << endl;
    cout << "different scores of matching functions" << std::endl;
    cout << "--------------------------------------------------" << endl;

    po::options_description desc("Allowed options");
    std::string gt_file;
    std::string odom_file;
    std::string base_name_pcd;    
    Eigen::Vector3d transl;
    Eigen::Vector3d euler;
    double resolution;
    int idx1, idx2;
    int dimidx1, dimidx2;
    int offset_size;
    double incr_dist;
    double incr_ang;
    std::string out_file;
    // Simply to make it transparant to the fuser node.
    lslgeneric::MotionModel2d::Params motion_params;
    int iter_step;
    int iters;

    desc.add_options()
        ("help", "produce help message")
	("gt_file", po::value<std::string>(&gt_file), "vehicle pose files in world frame")
	("odom_file", po::value<std::string>(&odom_file)->default_value(std::string("")), "estimated sensor poses (from egomotion) to be used in SC in world frame")
	("base_name_pcd", po::value<string>(&base_name_pcd)->default_value(std::string("")), "prefix for the .pcd files")
        ("out_file", po::value<string>(&out_file)->default_value(std::string("scores")), "prefix for the output files")
        ("idx1", po::value<int>(&idx1)->default_value(0), "'fixed' index of pose/pointcloud to be used")
        ("idx2", po::value<int>(&idx2)->default_value(1), "'moving' index of pose/pointcloud to be used")
        ("dimidx1", po::value<int>(&dimidx1)->default_value(0), "dimension index_x (what to plot on the X axis in gnuplot - 0,1,2,3,4,5 -> x,y,z,roll,pitch,yaw)")
        ("dimidx2", po::value<int>(&dimidx2)->default_value(1), "dimension index_y (what to plot on the Y axis in gnuplot - 0,1,2,3,4,5 -> x,y,z,roll,pitch,yaw)")
    
        ("x", po::value<double>(&transl[0])->default_value(0.), "sensor pose - translation vector x")
	("y", po::value<double>(&transl[1])->default_value(0.), "sensor pose - translation vector y")
	("z", po::value<double>(&transl[2])->default_value(0.), "sensor pose - translation vector z")
	("ex", po::value<double>(&euler[0])->default_value(0.), "sensor pose - euler angle vector x")
	("ey", po::value<double>(&euler[1])->default_value(0.), "sensor pose - euler angle vector y")
	("ez", po::value<double>(&euler[2])->default_value(0.), "sensor pose - euler angle vector z")
        ("resolution", po::value<double>(&resolution)->default_value(1.), "resolution of the map")
        ("offset_size", po::value<int>(&offset_size)->default_value(100), "2*number of evaluated points per dimension")
        ("incr_dist", po::value<double>(&incr_dist)->default_value(0.01), "incremental steps for distance directions (x,y,z)")
        ("incr_ang", po::value<double>(&incr_ang)->default_value(0.002), "incremental steps for angular directions (R,P,Y)")

        ("Dd", po::value<double>(&motion_params.Dd)->default_value(1.), "forward uncertainty on distance traveled")
        ("Dt", po::value<double>(&motion_params.Dt)->default_value(1.), "forward uncertainty on rotation")
        ("Cd", po::value<double>(&motion_params.Cd)->default_value(1.), "side uncertainty on distance traveled")
        ("Ct", po::value<double>(&motion_params.Ct)->default_value(1.), "side uncertainty on rotation")
        ("Td", po::value<double>(&motion_params.Td)->default_value(1.), "rotation uncertainty on distance traveled")
        ("Tt", po::value<double>(&motion_params.Tt)->default_value(1.), "rotation uncertainty on rotation")
        ("use_score_d2d_sc", "if the d2d sc score should be used in the objective plot")
        ("save_eps", "if .eps should be generated")
        ("save_pcd", "if .pcd point cloud should be generated gt, odom, d2d, d2d_sc...")
        ("save_global_Ts", "if global transforms on registrations odom, etc. should be generate, for evaluation with ate.py, rpe.py")
        ("iter_all_poses", "iterate over all available poses")
        ("iter_step", po::value<int>(&iter_step)->default_value(1), "iter step size")
        ("iters", po::value<int>(&iters)->default_value(1), "if additional iters should be evaluated (idx1 + iter, idx2 + iter)")
        ("gnuplot", "if gnuplot should be invoked")
        ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
	cout << desc << "\n";
	return 1;
    }
    if (odom_file.empty()) {
        cout << "odom_file not specified" << endl;
        return 1;
    }

    bool use_score_d2d_sc = vm.count("use_score_d2d_sc");
    bool save_eps = vm.count("save_eps");
    bool save_pcd = vm.count("save_pcd");
    bool save_global_Ts = vm.count("save_global_Ts");
    bool iter_all_poses = vm.count("iter_all_poses");
    bool use_gnuplot = vm.count("gnuplot");

    Eigen::Affine3d sensor_pose = ndt_generic::vectorsToAffine3d(transl,euler);
    std::cout << "Sensor pose used : " << ndt_generic::affine3dToStringRPY(sensor_pose) << std::endl;

    lslgeneric::MotionModel3d::Params motion_params3d(motion_params);

    std::cout << "Motion params used : " << motion_params3d << std::endl;

    NDTD2DSCScoreEval se(gt_file, odom_file, base_name_pcd, sensor_pose, motion_params);
    se.resolution = resolution;
    se.offset_size = offset_size;
    se.incr_dist = incr_dist;
    se.incr_ang = incr_ang;

    if (iter_all_poses) {
        iters = se.nbPoses();
    }

    if (iters > se.nbPoses()) {
        std::cerr << "[iters too large] # poses : " << se.nbPoses() << std::endl;
        exit(-1);
    }

    int i = 0;
    while (i < iters) {
        int _idx1 = idx1 + i;
        int _idx2 = idx2 + i;
        std::cout << "computing scores" << std::endl;
        se.computeScoreSets(_idx1, _idx2, dimidx1, dimidx2);
        
        std::cout << "saving : " << out_file << std::endl;
        se.save2D(out_file + ".dat", dimidx1, dimidx2);
        se.savePoseEstInOdomFrame(out_file + ".T");
        
        if (save_pcd) {
            std::string out_file_pcd = out_file + ndt_generic::toString(_idx1) + "_" + ndt_generic::toString(_idx2);
            se.savePCD(out_file_pcd);
        }
       
        if (use_gnuplot) {
            // Show it using gnuplot
            Gnuplot gp;//(std::fopen("output.gnuplot", "wb"));
            
            if (save_eps) {
                std::string out_file_eps = out_file + ndt_generic::toString(_idx1) + "_" + ndt_generic::toString(_idx2) + "x" + ndt_generic::toString(dimidx1) + "y" + ndt_generic::toString(dimidx2) + "sc" + ndt_generic::toString(use_score_d2d_sc) + ".eps";
                std::cout << "saving : " << out_file_eps << std::endl;
                gp << "set terminal postscript eps size 3.5,2.62 enhanced color font 'Helvetica,12' lw 1\n";
                gp << "set output '" << out_file_eps << "'\n";
            }
            gp << "set title \"Objective score values\"\n";
            gp << "set key top right\n";
            gp << "set pm3d map\n";
            
            gp << "splot '-' with pm3d notitle, '-' with points pt 1 title 'GT', '-' with points pt 2 title 'd2d', '-' with points pt 3 title 'd2d sc', '-' with points pt 4 title 'icp', '-' with points pt 2 title 'filter', '-' with points pt 4 title 'icp filter'\n";
            gp.send2d( se.getScoreSegments(dimidx1, dimidx2, use_score_d2d_sc));
            gp.send1d( se.getRelPoseGT(dimidx1, dimidx2));
            gp.send1d( se.getRelPoseD2D(dimidx1, dimidx2));
            gp.send1d( se.getRelPoseD2D_SC(dimidx1, dimidx2));
            gp.send1d( se.getRelPoseICP(dimidx1, dimidx2));
            gp.send1d( se.getRelPoseFilter(dimidx1, dimidx2));
            gp.send1d( se.getRelPoseICPFilter(dimidx1, dimidx2));
        }
        i += iter_step;
    }

    if (save_global_Ts) {
        // Saves all transforms 
        std::string out_file_Ts = out_file + ".Ts";
        se.saveTsToEvalFiles(out_file_Ts);
    }
    std::cout << "done." << std::endl;
}
