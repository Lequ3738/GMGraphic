#pragma once

#include "..\main.h"

double roundd(double x);
double frac(double x);
bool   eps(double a, double b);
bool   eps2(double a, double b, double eps);
double slope(double x1, double y1, double x2, double y2);
double lerp(double vala, double valb, double x);
double cerp(double vala, double valb, double x);
double curp(double a, double b, double c, double d, double x);
double unlerp(double val, double minval, double maxval);
double clamp(double val, double minval, double maxval);
double snap_low(double x, double cellw);
uint   snap_low(uint   x, uint   cellw);
double snap_high(double x, double cellw);
double snap_near(double x, double cellw);
void   tr_rot(double& x, double& y, double dir);
void   tr_move(double& x, double& y, double len, double dir);
void   tr_scale(double& x, double& y, double scale);
int    col_red(int col);
int    col_green(int col);
int    col_blue(int col);
int    col_make(int r, int g, int b);
int    col_lerp(int cola, int colb, double a);
uint   col_d3d(int gmcol, double gmalpha);

bool   file_read(char* file_in, char*& target, uint& size);
bool   file_write(char* file_out, void* data, uint size, bool append = false);
bool   file_write(char* file_out, std::string& data, uint size, bool append = false);