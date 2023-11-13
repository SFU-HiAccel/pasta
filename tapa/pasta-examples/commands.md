To compile with g++:
```bash
g++ -o vadd -O2 src/add.cpp src/add-host.cpp -ltapa -lfrt -lglog -lgflags -lOpenCL -std=c++17 -DTAPA_BUFFER_SUPPORT
```

To compile with tapac:
```cpp
tapac src/add.cpp --platform xilinx_u280_xdma_201920_3 --run-tapacc --run-hls --generate-task-rtl --generate-top-rtl --pack-xo -o vadd.xo --top VecAdd --enable-buffer-support --work-dir run --connectivity connectivity.ini --run-floorplanning --floorplan-output constraints.tcl
```
