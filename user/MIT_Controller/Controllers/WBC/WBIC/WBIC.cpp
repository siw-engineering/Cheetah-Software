#include "WBIC.hpp"
#include <Utilities/Timer.h>
#include <eigen3/Eigen/LU>
#include <eigen3/Eigen/SVD>
#include <qpOASES.hpp>
#include <fstream>
#include "json.hpp"
#include "eigenConversion.hpp"

static unsigned int someCount = 0;
const int printIndex = 600;
static nlohmann::json dataJson;

template<typename T>
WBIC<T>::WBIC(size_t num_qdot, const std::vector<ContactSpec<T> *> *contact_list,
              const std::vector<Task<T> *> *task_list)
        : WBC<T>(num_qdot), _dim_floating(6) {
    _contact_list = contact_list;
    _task_list = task_list;

    _eye = DMat<T>::Identity(WB::num_qdot_, WB::num_qdot_);
    _eye_floating = DMat<T>::Identity(_dim_floating, _dim_floating);
}

template<typename T>
void WBIC<T>::MakeTorque(DVec<T> &cmd, void *extra_input) {
    if (!WB::b_updatesetting_) {
        printf("[Wanning] WBIC setting is not done\n");
    }
    if (extra_input) _data = static_cast<WBIC_ExtraData<T> *>(extra_input);

    // resize G, g0, CE, ce0, CI, ci0
    _SetOptimizationSize();
    _SetCost();

    DVec<T> qddot_pre;
    DMat<T> JcBar;
    DMat<T> Npre;

    if (_dim_rf > 0) {
        // Contact Setting
        _ContactBuilding();

        // Set inequality constraints
        _SetInEqualityConstraint();
        WB::_WeightedInverse(_Jc, WB::Ainv_, JcBar);
        qddot_pre = JcBar * (-_JcDotQdot);
        Npre = _eye - JcBar * _Jc;
        // pretty_print(JcBar, std::cout, "JcBar");
        // pretty_print(_JcDotQdot, std::cout, "JcDotQdot");
        // pretty_print(qddot_pre, std::cout, "qddot 1");
    } else {
        qddot_pre = DVec<T>::Zero(WB::num_qdot_);
        Npre = _eye;
    }

    // Task
    Task<T> *task;
    DMat<T> Jt, JtBar, JtPre;
    DVec<T> JtDotQdot, xddot;

    for (size_t i(0); i < (*_task_list).size(); ++i) {
        task = (*_task_list)[i];

        task->getTaskJacobian(Jt);
        task->getTaskJacobianDotQdot(JtDotQdot);
        task->getCommand(xddot);

        JtPre = Jt * Npre;
        WB::_WeightedInverse(JtPre, WB::Ainv_, JtBar);

        qddot_pre += JtBar * (xddot - JtDotQdot - Jt * qddot_pre);
        Npre = Npre * (_eye - JtBar * JtPre);

        // pretty_print(xddot, std::cout, "xddot");
        // pretty_print(JtDotQdot, std::cout, "JtDotQdot");
        // pretty_print(qddot_pre, std::cout, "qddot 2");
        // pretty_print(Jt, std::cout, "Jt");
        // pretty_print(JtPre, std::cout, "JtPre");
        // pretty_print(JtBar, std::cout, "JtBar");
    }

    // Set equality constraints
    _SetEqualityConstraint(qddot_pre);

    // printf("G:\n");
    // std::cout<<G<<std::endl;
    // printf("g0:\n");
    // std::cout<<g0<<std::endl;

    // Optimization
    // Timer timer;
    auto createMat = [](const GolDIdnani::GMatr<double> &input) -> Eigen::Matrix<double, -1, -1, Eigen::RowMajor> {
        auto nRows = input.nrows();
        auto nCols = input.ncols();
        Eigen::Matrix<double, -1, -1, Eigen::RowMajor> mat = Eigen::Matrix<double, -1, -1, Eigen::RowMajor>::Zero(nRows, nCols);
        for (auto i = 0ul; i < nRows; i++) {
            for (auto j = 0ul; j < nCols; j++) {
                mat(i, j) = input[i][j];
            }
        }

        return mat;
    };
    auto createVec = [](const GolDIdnani::GVect<double> &input) -> Eigen::VectorXd {
        auto nRows = input.size();
        Eigen::VectorXd mat = Eigen::VectorXd::Zero(nRows);
        for (auto i = 0ul; i < nRows; i++) {
            mat(i) = input[i];
        }

        return mat;
    };
    Eigen::Matrix<double, -1, -1, Eigen::RowMajor> G_eigen = createMat(G);
    Eigen::VectorXd g0_eigen = createVec(g0);
    Eigen::Matrix<double, -1, -1, Eigen::RowMajor> CE_eigen = createMat(CE).transpose();
    Eigen::VectorXd ce0_eigen = createVec(ce0) * -1;
    Eigen::Matrix<double, -1, -1, Eigen::RowMajor> CI_eigen = createMat(CI).transpose();
    Eigen::VectorXd ci0_eigen = createVec(ci0) * -1;

    qpOASES::QProblem example(_dim_opt, _dim_eq_cstr + _dim_rf);

    qpOASES::Options options;
    options.printLevel = qpOASES::PL_NONE;
    example.setOptions(options);
    /* Solve first QP. */
    qpOASES::int_t nWSR = 1000;

    Eigen::Matrix<double, -1, -1, Eigen::RowMajor> constraints(CE_eigen.rows() + CI_eigen.rows(), CE_eigen.cols());
    constraints << CE_eigen, CI_eigen;

    auto doubleMax = std::numeric_limits<double>::max();
    auto doubleMin = std::numeric_limits<double>::lowest();
    Eigen::VectorXd lowerBounds = Eigen::VectorXd(ce0_eigen.size() + ci0_eigen.size());
    lowerBounds << ce0_eigen, ci0_eigen;
    Eigen::VectorXd upperBounds = Eigen::VectorXd(ce0_eigen.size() + ci0_eigen.size());
    upperBounds << ce0_eigen, Eigen::VectorXd::Ones(ci0_eigen.size()) * doubleMax;
    example.init(G_eigen.data(), g0_eigen.data(), constraints.data(), NULL, NULL, lowerBounds.data(), upperBounds.data(), nWSR);
    Eigen::Matrix<double, -1, 1> solved_x = Eigen::Matrix<double, -1, 1>(_dim_opt);
    example.getPrimalSolution(solved_x.data());

    Eigen::IOFormat CommaInitFmt(Eigen::StreamPrecision, Eigen::DontAlignCols, ", ", ", ", "", "", "", ";");

//    T f = solve_quadprog(G, g0, CE, ce0, CI, ci0, z);
    z.resize(G.ncols());
    // std::cout<<"\n wbic old time: "<<timer.getMs()<<std::endl;
//    (void)f;

    for (auto i = 0ul; i < _dim_opt; i++) {
        z[i] = solved_x[i];
    }
    // std::cout<<"\n wbic old time: "<<timer.getMs()<<std::endl;

    // pretty_print(qddot_pre, std::cout, "qddot_cmd");
    for (size_t i(0); i < _dim_floating; ++i) qddot_pre[i] += z[i];
    _GetSolution(qddot_pre, cmd);

    _data->_opt_result = DVec<T>(_dim_opt);
    for (size_t i(0); i < _dim_opt; ++i) {
        _data->_opt_result[i] = z[i];
    }
    if(someCount == printIndex){
        std::cout << "Saving data2" << std::endl;
        nlohmann::json jG, jg0, jCE, jCI, jci0, jce0, jz;
        Eigen::to_json(jG, G_eigen);
        Eigen::to_json(jg0, g0_eigen);
        Eigen::to_json(jCE, CE_eigen);
        Eigen::to_json(jCI, CI_eigen);
        Eigen::to_json(jci0, ci0_eigen);
        Eigen::to_json(jce0, ce0_eigen);
        Eigen::to_json(jz, solved_x);
        dataJson["G"] = jG;
        dataJson["g0"] = jg0;
        dataJson["CE"] = jCE;
        dataJson["CI"] = jCI;
        dataJson["ci0"] = jci0;
        dataJson["ce0"] = jce0;
        dataJson["z"] = jz;

// write prettified JSON to another file
//        std::ofstream o("/home/pohzhiee/Documents/hyq_cheetah_ws/src/hyq_cheetah/src/test/data/data_trot.json");
//        o << std::setw(4) << dataJson << std::endl;
    }
    // std::cout << "f: " << f << std::endl;
    //std::cout << "x: " << z << std::endl;

    // DVec<T> check_eq = _dyn_CE * _data->_opt_result + _dyn_ce0;
    // pretty_print(check_eq, std::cout, "equality constr");
    // std::cout << "cmd: "<<cmd<<std::endl;
    // pretty_print(qddot_pre, std::cout, "qddot_pre");
    // pretty_print(JcN, std::cout, "JcN");
    // pretty_print(Nci_, std::cout, "Nci");
    // DVec<T> eq_check = dyn_CE * data_->opt_result_;
    // pretty_print(dyn_ce0, std::cout, "dyn ce0");
    // pretty_print(eq_check, std::cout, "eq_check");

    // pretty_print(Jt, std::cout, "Jt");
    // pretty_print(JtDotQdot, std::cout, "Jtdotqdot");
    // pretty_print(xddot, std::cout, "xddot");

    // printf("CE:\n");
    // std::cout<<CE<<std::endl;
    // printf("ce0:\n");
    // std::cout<<ce0<<std::endl;

    // printf("CI:\n");
    // std::cout<<CI<<std::endl;
    // printf("ci0:\n");
    // std::cout<<ci0<<std::endl;
//    std::cout << "someCount: " << someCount << std::endl;
    ++someCount;
}

template<typename T>
void WBIC<T>::_SetEqualityConstraint(const DVec<T> &qddot) {
    if (_dim_rf > 0) {
        _dyn_CE.block(0, 0, _dim_eq_cstr, _dim_floating) =
                WB::A_.block(0, 0, _dim_floating, _dim_floating);
        _dyn_CE.block(0, _dim_floating, _dim_eq_cstr, _dim_rf) =
                -WB::Sv_ * _Jc.transpose();
        _dyn_ce0 = -WB::Sv_ * (WB::A_ * qddot + WB::cori_ + WB::grav_ -
                               _Jc.transpose() * _Fr_des);
    } else {
        _dyn_CE.block(0, 0, _dim_eq_cstr, _dim_floating) =
                WB::A_.block(0, 0, _dim_floating, _dim_floating);
        _dyn_ce0 = -WB::Sv_ * (WB::A_ * qddot + WB::cori_ + WB::grav_);
    }

    for (size_t i(0); i < _dim_eq_cstr; ++i) {
        for (size_t j(0); j < _dim_opt; ++j) {
            CE[j][i] = _dyn_CE(i, j);
        }
        ce0[i] = -_dyn_ce0[i];
    }
    if(someCount == printIndex){
        std::cout << "Saving data" << std::endl;
        nlohmann::json A, b, g, Sf, Jc, qddot_cmd, fr_mpc;
        Eigen::to_json(A, WB::A_);
        Eigen::to_json(b, WB::cori_);
        Eigen::to_json(g, WB::grav_);
        Eigen::to_json(Sf, WB::Sv_);
        Eigen::to_json(Jc, _Jc);
        Eigen::to_json(qddot_cmd, qddot);
        Eigen::to_json(fr_mpc, _Fr_des);
        dataJson["A"] = A;
        dataJson["b"] = b;
        dataJson["g"] = g;
        dataJson["Sf"] = Sf;
        dataJson["Jc"] = Jc;
        dataJson["qddot_cmd"] = qddot_cmd;
        dataJson["fr_mpc"] = fr_mpc;
    }
    // pretty_print(_dyn_CE, std::cout, "WBIC: CE");
    // pretty_print(_dyn_ce0, std::cout, "WBIC: ce0");
}

template<typename T>
void WBIC<T>::_SetInEqualityConstraint() {
    _dyn_CI.block(0, _dim_floating, _dim_Uf, _dim_rf) = _Uf;
    _dyn_ci0 = _Uf_ieq_vec - _Uf * _Fr_des;

    for (size_t i(0); i < _dim_Uf; ++i) {
        for (size_t j(0); j < _dim_opt; ++j) {
            CI[j][i] = _dyn_CI(i, j);
        }
        ci0[i] = -_dyn_ci0[i];
    }

    if(someCount == printIndex){
        nlohmann::json Uf, Uf_ieq_vec;
        Eigen::to_json(Uf, _Uf);
        Eigen::to_json(Uf_ieq_vec, _Uf_ieq_vec);
        dataJson["Uf"] = Uf;
        dataJson["Uf_ieq_vec"] = Uf_ieq_vec;
    }
    // pretty_print(_dyn_CI, std::cout, "WBIC: CI");
    // pretty_print(_dyn_ci0, std::cout, "WBIC: ci0");
}

template<typename T>
void WBIC<T>::_ContactBuilding() {
    DMat<T> Uf;
    DVec<T> Uf_ieq_vec;
    // Initial
    DMat<T> Jc;
    DVec<T> JcDotQdot;
    size_t dim_accumul_rf, dim_accumul_uf;
    (*_contact_list)[0]->getContactJacobian(Jc);
    (*_contact_list)[0]->getJcDotQdot(JcDotQdot);
    (*_contact_list)[0]->getRFConstraintMtx(Uf);
    (*_contact_list)[0]->getRFConstraintVec(Uf_ieq_vec);

    dim_accumul_rf = (*_contact_list)[0]->getDim();
    dim_accumul_uf = (*_contact_list)[0]->getDimRFConstraint();

    _Jc.block(0, 0, dim_accumul_rf, WB::num_qdot_) = Jc;
    _JcDotQdot.head(dim_accumul_rf) = JcDotQdot;
    _Uf.block(0, 0, dim_accumul_uf, dim_accumul_rf) = Uf;
    _Uf_ieq_vec.head(dim_accumul_uf) = Uf_ieq_vec;
    _Fr_des.head(dim_accumul_rf) = (*_contact_list)[0]->getRFDesired();

    size_t dim_new_rf, dim_new_uf;

    for (size_t i(1); i < (*_contact_list).size(); ++i) {
        (*_contact_list)[i]->getContactJacobian(Jc);
        (*_contact_list)[i]->getJcDotQdot(JcDotQdot);

        dim_new_rf = (*_contact_list)[i]->getDim();
        dim_new_uf = (*_contact_list)[i]->getDimRFConstraint();

        // Jc append
        _Jc.block(dim_accumul_rf, 0, dim_new_rf, WB::num_qdot_) = Jc;

        // JcDotQdot append
        _JcDotQdot.segment(dim_accumul_rf, dim_new_rf) = JcDotQdot;

        // Uf
        (*_contact_list)[i]->getRFConstraintMtx(Uf);
        _Uf.block(dim_accumul_uf, dim_accumul_rf, dim_new_uf, dim_new_rf) = Uf;

        // Uf inequality vector
        (*_contact_list)[i]->getRFConstraintVec(Uf_ieq_vec);
        _Uf_ieq_vec.segment(dim_accumul_uf, dim_new_uf) = Uf_ieq_vec;

        // Fr desired
        _Fr_des.segment(dim_accumul_rf, dim_new_rf) =
                (*_contact_list)[i]->getRFDesired();
        dim_accumul_rf += dim_new_rf;
        dim_accumul_uf += dim_new_uf;
    }
    //pretty_print(_Fr_des, std::cout, "[WBIC] Fr des");
    // pretty_print(_Jc, std::cout, "[WBIC] Jc");
    // pretty_print(_JcDotQdot, std::cout, "[WBIC] JcDot Qdot");
    // pretty_print(_Uf, std::cout, "[WBIC] Uf");
}

template<typename T>
void WBIC<T>::_GetSolution(const DVec<T> &qddot, DVec<T> &cmd) {
    DVec<T> tot_tau;
    if (_dim_rf > 0) {
        _data->_Fr = DVec<T>(_dim_rf);
        // get Reaction forces
        for (size_t i(0); i < _dim_rf; ++i)
            _data->_Fr[i] = z[i + _dim_floating] + _Fr_des[i];
        tot_tau =
                WB::A_ * qddot + WB::cori_ + WB::grav_ - _Jc.transpose() * _data->_Fr;

    } else {
        tot_tau = WB::A_ * qddot + WB::cori_ + WB::grav_;
    }
    _data->_qddot = qddot;
    cmd = tot_tau.tail(WB::num_act_joint_);

    if(someCount == printIndex) {
        nlohmann::json jCmd, jTau, jqddot, jFr;
        Eigen::to_json(jCmd, cmd);
        Eigen::to_json(jTau, tot_tau);
        Eigen::to_json(jqddot, qddot);
        Eigen::to_json(jFr, _data->_Fr);
        dataJson["cmd"] = jCmd;
        dataJson["tau"] = jTau;
        dataJson["qddot_final"] = jqddot;
        dataJson["fr_final"] = jFr;
    }

    // Torque check
    // DVec<T> delta_tau = DVec<T>::Zero(WB::num_qdot_);
    // for(size_t i(0); i<_dim_floating; ++i) delta_tau[i] = z[i];
    // pretty_print(tot_tau, std::cout, "tot tau original");
    // tot_tau += delta_tau;
    // pretty_print(tot_tau, std::cout, "tot tau result");
    // pretty_print(qddot, std::cout, "qddot");
    // pretty_print(_data->_Fr, std::cout, "Fr");
    // pretty_print(_Fr_des, std::cout, "Fr des");
}

template<typename T>
void WBIC<T>::_SetCost() {
    // Set Cost
    size_t idx_offset(0);
    for (size_t i(0); i < _dim_floating; ++i) {
        G[i + idx_offset][i + idx_offset] = _data->_W_floating[i];
    }
    idx_offset += _dim_floating;
    for (size_t i(0); i < _dim_rf; ++i) {
        G[i + idx_offset][i + idx_offset] = _data->_W_rf[i];
    }
    // pretty_print(_data->_W_floating, std::cout, "W floating");
    // pretty_print(_data->_W_rf, std::cout, "W rf");
}

template<typename T>
void WBIC<T>::UpdateSetting(const DMat<T> &A, const DMat<T> &Ainv,
                            const DVec<T> &cori, const DVec<T> &grav,
                            void *extra_setting) {
    WB::A_ = A;
    WB::Ainv_ = Ainv;
    WB::cori_ = cori;
    WB::grav_ = grav;
    WB::b_updatesetting_ = true;

    (void) extra_setting;
}

template<typename T>
void WBIC<T>::_SetOptimizationSize() {
    // Dimension
    _dim_rf = 0;
    _dim_Uf = 0;  // Dimension of inequality constraint
    for (size_t i(0); i < (*_contact_list).size(); ++i) {
        _dim_rf += (*_contact_list)[i]->getDim();
        _dim_Uf += (*_contact_list)[i]->getDimRFConstraint();
    }

    _dim_opt = _dim_floating + _dim_rf;
    _dim_eq_cstr = _dim_floating;

    // Matrix Setting
    G.resize(0., _dim_opt, _dim_opt);
    g0.resize(0., _dim_opt);
    CE.resize(0., _dim_opt, _dim_eq_cstr);
    ce0.resize(0., _dim_eq_cstr);

    // Eigen Matrix Setting
    _dyn_CE = DMat<T>::Zero(_dim_eq_cstr, _dim_opt);
    _dyn_ce0 = DVec<T>(_dim_eq_cstr);
    if (_dim_rf > 0) {
        CI.resize(0., _dim_opt, _dim_Uf);
        ci0.resize(0., _dim_Uf);
        _dyn_CI = DMat<T>::Zero(_dim_Uf, _dim_opt);
        _dyn_ci0 = DVec<T>(_dim_Uf);

        _Jc = DMat<T>(_dim_rf, WB::num_qdot_);
        _JcDotQdot = DVec<T>(_dim_rf);
        _Fr_des = DVec<T>(_dim_rf);

        _Uf = DMat<T>(_dim_Uf, _dim_rf);
        _Uf.setZero();
        _Uf_ieq_vec = DVec<T>(_dim_Uf);
    } else {
        CI.resize(0., _dim_opt, 1);
        ci0.resize(0., 1);
    }
}

template
class WBIC<double>;

template
class WBIC<float>;
