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
