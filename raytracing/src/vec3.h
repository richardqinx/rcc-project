/*
 * Copyright (C) 2026 Richard
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef VEC_3
#define VEC_3

#include <array>
#include <cmath>
#include <iostream>

class vec3
{
public:
  std::array<double, 3> e;

  vec3() : e { 0, 0, 0 } {}
  vec3( double e0, double e1, double e2 ) : e { e0, e1, e2 } {}

  double x() const;
  double y() const;
  double z() const;

  vec3 operator-() const;
  double operator[]( int i ) const;
  double& operator[]( int i );
  vec3& operator+=( const vec3& other );
  vec3& operator*=( double t );
  vec3& operator/=( double t );
  double length() const;
  double length_squared() const;
};

// point3 is just an alias for vec3, but useful for geometric clarity in the code.
using point3 = vec3;

// Vector Utility Functions

inline std::ostream& operator<<( std::ostream& out, const vec3& v )
{
  return out << v.e[0] << ' ' << v.e[1] << ' ' << v.e[2];
}

inline vec3 operator+( const vec3& u, const vec3& v )
{
  return { u.e[0] + v.e[0], u.e[1] + v.e[1], u.e[2] + v.e[2] };
}

inline vec3 operator-( const vec3& u, const vec3& v )
{
  return { u.e[0] - v.e[0], u.e[1] - v.e[1], u.e[2] - v.e[2] };
}

inline vec3 operator*( const vec3& u, const vec3& v )
{
  return { u.e[0] * v.e[0], u.e[1] * v.e[1], u.e[2] * v.e[2] };
}

inline vec3 operator*( double t, const vec3& v )
{
  return { t * v.e[0], t * v.e[1], t * v.e[2] };
}

inline vec3 operator*( const vec3& v, double t )
{
  return t * v;
}

inline vec3 operator/( const vec3& v, double t )
{
  return ( 1 / t ) * v;
}

inline double dot( const vec3& u, const vec3& v )
{
  return u.e[0] * v.e[0] + u.e[1] * v.e[1] + u.e[2] * v.e[2];
}

inline vec3 cross( const vec3& u, const vec3& v )
{
  return {
    u.e[1] * v.e[2] - u.e[2] * v.e[1], u.e[2] * v.e[0] - u.e[0] * v.e[2], u.e[0] * v.e[1] - u.e[1] * v.e[0] };
}

inline vec3 unit_vector( const vec3& v )
{
  return v / v.length();
}

#endif
