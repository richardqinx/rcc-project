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
#include "vec3.h"

double vec3::x() const
{
  return e[0];
}
double vec3::y() const
{
  return e[1];
}
double vec3::z() const
{
  return e[2];
}

vec3 vec3::operator-() const
{
  return { -e[0], -e[1], -e[2] };
}

double vec3::operator[]( int i ) const
{
  return e.at( i );
}

double& vec3::operator[]( int i )
{
  return e.at( i );
}

vec3& vec3::operator+=( const vec3& other )
{
  e[0] += other.e[0];
  e[1] += other.e[1];
  e[2] += other.e[2];
  return *this;
}

vec3& vec3::operator*=( double t )
{
  e[0] *= t;
  e[1] *= t;
  e[2] *= t;
  return *this;
}

vec3& vec3::operator/=( double t )
{
  return *this *= 1 / t;
}

double vec3::length() const
{
  return std::sqrt( length_squared() );
}

double vec3::length_squared() const
{
  return e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
}

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
