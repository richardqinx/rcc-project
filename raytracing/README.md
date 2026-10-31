# Ray Tracer

Inspired by the book _Ray Tracing in One Weekend_, I'd like to write this project using modern C++ (C++ 23) and attempt to use high-performance approaches.

## Build & Run

Make Sure your build system is properly set up:
```bash
cmake -S . -B build
```

Configure for Release:
```bash
# Release mode
cmake -B build -DCMAKE_BUILD_TYPE=Release
#Debug mode
cmake -B build -DCMAKE_BUILD_TYPE=Debug
```

Compile the source code:
```bash
cmake --build build
```

To normalize the coding style
```bash
cmake --build build --target format
```

Suggests improvements to follow good C++ programming practices:
```bash
cmake --build build --target tidy
cmake --build build --target tidy-all
```

Run the program
```bash
./build/apps/RayTracer > images/image.ppm
display images/image.ppm
```

## License
This project is licensed under the **GNU General Public License v3.0**. 
See the [LICENSE](LICENSE) file for the full text.