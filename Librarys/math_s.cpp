#include "math_s.h"

double roundd(double x)
{
	return ((x < 0.0) ? ceil(x - 0.5) : floor(x + 0.5));
}

double frac(double x) { return fmod(x, 1.0); }
bool eps(double a, double b = 0.0) { return (fabs(a - b) < epsilon); }
bool eps2(double a, double b, double eps) { return (fabs(a - b) < eps); }

// Slope of a line
double slope(double x1, double y1, double x2, double y2)
{
	double dx;
	dx = x2 - x1;

	if (eps(dx))
		return 0.0;
	else
		return (y2 - y1) / dx;
}

// Linear interpolation.
double lerp(double vala, double valb, double x)
{
	return vala + ((valb - vala) * x);
}

// Cosine interpolation.
double cerp(double vala, double valb, double x)
{
	double am2 = (1 - cos(x * pi)) * 0.5;
	return vala + ((valb - vala) * am2);
}

// Cubic interpolation.  0-1 == B-C.
// A and D are the datapoints before and after those.
double curp(double a, double b, double c, double d, double x)
{
	double p, q;
	p = (d - c) - (a - b);
	q = (a - b) - p;

	return (p * x * x * x) + (q * x * x) + ((c - a) * x) + b;
}

// Returns the value normalised into the range 0-1.
double unlerp(double val, double minval, double maxval)
{
	return (val - minval) / (maxval - minval);
}

double clamp(double val, double minval, double maxval)
{
	if (val < minval)
		return minval;
	else if (val > maxval)
		return maxval;

	return val;
}

double snap_low(double x, double cellw) { return x - fmod(x, cellw); }
uint snap_low(uint x, uint cellw) { return x - (x % cellw); }
double snap_high(double x, double cellw) { return (x - fmod(x, cellw)) + cellw; }
double snap_near(double x, double cellw) { return roundd(x / cellw) * cellw; }

// Rotate points around 0,0.
void tr_rot(double& x, double& y, double dir)
{
	double rad, s, c;
	rad = dir * degtorad_mul;
	s = -sin(rad);
	c = cos(rad);
	x = (x * c) - (y * s);
	y = (x * s) + (y * c);
}

// Move point with polar coordinates. Same as lengthdir.
void tr_move(double& x, double& y, double len, double dir)
{
	double rad;
	rad = dir * degtorad_mul;
	x += cos(rad) * len;
	y += -sin(rad) * len;
}

void tr_scale(double& x, double& y, double scale)
{
	x *= scale;
	y *= scale;
}

int col_red(int col) { return (col % 256); }
int col_green(int col) { return ((col >> 8) % 256); }
int col_blue(int col) { return (col >> 16); }
int col_make(int r, int g, int b) { return (b << 16) + (g << 8) + r; }

// Lerp between colours.  Same as merge_color in GML.
int col_lerp(int cola, int colb, double a)
{
	
	int r, g, b;
	r = (int)lerp(col_red(cola), col_red(colb), a);
	g = (int)lerp(col_green(cola), col_green(colb), a);
	b = (int)lerp(col_blue(cola), col_blue(colb), a);

	return col_make(r, g, b);
}

// For some reason GM uses BGR instead of D3D's normal RGB format.
uint col_d3d(int gmcol, double gmalpha)
{
	uint r, g, b, a;
	r = col_red(gmcol);
	g = col_green(gmcol);
	b = col_blue(gmcol);
	a = uint(clamp(gmalpha, 0.0, 1.0) * 255.0);

	return (a << 24) + (r << 16) + (g << 8) + b;
}

uint d3dcol_to_col(d3dcolor color)
{
	int r = ((color >> 16) % 256);
	int g = ((color >> 8) % 256);
	int b = (color % 256);

	return col_make(r, g, b);
}
double d3dcol_to_alpha(d3dcolor color)
{
	return static_cast<double>((color & 0xFF000000) >> 24) / 255.0;
}

// Load file into RAM.  Declare target pointer && size vars first.
// Returns whether it succeeded; if it works, delete[] the buffer when you're done.
bool file_read(char* file_in, char*& target, uint& size)
{
	using namespace std;
	fstream f;

	f.open(file_in, ios::in | ios::binary);
	if (f.is_open())
	{
		f.seekg(0, ios::end);
		size = (uint)f.tellg();

		target = new (nothrow) char[size];
		if (target != nullptr)
		{
			f.seekg(0, ios::beg);
			f.read(target, size);
			f.close();
			return true;
		}
		else
		{
			f.close();
			return false;
		}

	}
	else
		return false;
}

// Write from RAM to file. Returns whether it succeeded.
bool file_write(char* file_out, void* data, uint size, bool append)
{
	
	using namespace std;
	fstream f;

	if (append) { f.open(file_out, ios::out | ios::binary | ios::app); }
	else { f.open(file_out, ios::out | ios::binary); }

	if (f.is_open())
	{
		f.write((char*)data, size);
		f.close();
	}
	else { return false; }

	return true;
}

bool file_write(char* file_out, std::string& data, uint size, bool append)
{
	return file_write(file_out, data.data(), std::min(size, (uint)data.length()), append);
}