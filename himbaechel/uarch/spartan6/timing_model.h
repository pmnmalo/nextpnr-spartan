// Generated from Silinx's packages/silinx-fpga/src/fpga/s6/timing-model.js (S6_SPEED_3.arcs, the -3 speed grade) by
// Silinx's research/nextpnr-s6/export-arcs.mjs: trce's arc types in ns (base: the defaults; types: each type's median;
// classes: by site class). Not edited by hand.
#ifndef SPARTAN6_TIMING_MODEL_H
#define SPARTAN6_TIMING_MODEL_H
#include <map>
#include <string>
namespace s6_model {
inline const std::map<std::string, double> &base() { static const std::map<std::string, double> m = {{"Tilo", 0.259}, {"TiloMux", 0.261}, {"Topab", 0.469}, {"Topcyb", 0.375}, {"Tbyp", 0.076}, {"Tcinb", 0.26}, {"Tas", 0.322}, {"TasXor", 0.154}, {"Tdick", 0.136}, {"Tceck", 0.331}, {"Tsrck", 0.444}, {"Tcko", 0.408}, {"Tcinck", 0.3}, {"Tito", 0.75}, {"Tds", 0.04}, {"Tshcko", 0.461}}; return m; }
inline const std::map<std::string, double> &types() { static const std::map<std::string, double> m = {{"Tcko", 0.408}, {"Tilo", 0.259}, {"Tito_logic", 0.75}, {"Tas", 0.322}, {"Tad_logic", 0.884}, {"Tsrck", 0.444}, {"Topcya", 0.386}, {"Tbyp", 0.076}, {"Tcina", 0.202}, {"Tceck", 0.331}, {"Topcyd", 0.261}, {"Tdxcy", 0.087}, {"Tcinb", 0.26}, {"Trcko_DOB", 1.85}, {"Tdspdo_A_P", 3.346}, {"Tdick", 0.136}, {"Topbb", 0.376}, {"Topad", 0.55}, {"Tdxd", 0.256}, {"Topdd", 0.455}, {"Topac", 0.533}, {"Tcxd", 0.266}, {"Topab", 0.469}, {"Taxb", 0.255}, {"Topcyc", 0.277}, {"Topcyb", 0.375}, {"Tshcko", 0.461}, {"Tcinc", 0.272}, {"Tcxc", 0.163}, {"Tcind", 0.272}, {"Tdspdck_B_MREG", 3.108}}; return m; }
inline const std::map<std::string, std::map<std::string, double>> &classes() { static const std::map<std::string, std::map<std::string, double>> m = {{"CLEXL x", {{"Tcko", 0.391}, {"Tas", 0.322}, {"Tilo", 0.259}, {"Tceck", 0.324}}}, {"CLEXM lm", {{"TiloMux", 0.261}, {"Tilo", 0.203}, {"Tdick", 0.086}, {"Tcko", 0.447}, {"Tas", 0.289}, {"Tsrck", 0.439}, {"Tceck", 0.295}}}, {"CLEXM x", {{"Tas", 0.322}, {"Tcko", 0.391}, {"Tilo", 0.259}, {"Tceck", 0.361}, {"Tdick", 0.063}}}, {"CLEXL lm", {{"Tcko", 0.408}, {"Tilo", 0.205}, {"Tsrck", 0.444}, {"Tceck", 0.335}, {"Tas", 0.341}, {"Tdick", 0.136}}}}; return m; }
} // namespace s6_model
#endif
