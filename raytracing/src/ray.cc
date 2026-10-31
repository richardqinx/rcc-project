#include "ray.h"

const point3& ray::origin() const
{
  return this->orig;
}

const vec3& ray::direction() const
{
  return this->dir;
}

point3 ray::at( double t ) const
{
  return this->orig + t * this->dir;
}