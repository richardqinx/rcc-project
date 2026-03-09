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

void write_color( std::ostream& out, const color& pixel_color )
{
  auto red = pixel_color.x();
  auto green = pixel_color.y();
  auto blue = pixel_color.z();

  // Translate the [0,1] component values to the byte range [0,255].
  const int rbyte = static_cast<int>( 255.999 * red );
  const int gbyte = static_cast<int>( 255.999 * green );
  const int bbyte = static_cast<int>( 255.999 * blue );

  // Write out the pixel color components.
  out << rbyte << ' ' << gbyte << ' ' << bbyte << '\n';
}