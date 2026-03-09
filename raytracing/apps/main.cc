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
#include "color.h"
#include "vec3.h"

#include <iostream>

int main()
{
  // Image

  const int image_width = 256;
  const int image_height = 256;

  // Render

  std::cout << "P3\n" << image_width << " " << image_height << "\n255\n";
  for ( int j = 0; j < image_height; j++ ) {
    std::clog << "\rScanlines remaining: " << ( image_height - j ) << ' ' << std::flush;
    for ( int i = 0; i < image_width; i++ ) {
      auto pixel_color = color(
        static_cast<double>( i ) / ( image_width - 1 ), static_cast<double>( j ) / ( image_height - 1 ), 0 );
      write_color( std::cout, pixel_color );
    }
  }
  std::clog << "\rDone.                 \n";
  return 0;
}