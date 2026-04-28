.PHONY: all clean

all:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j
	cp build/libhypranimated.so hypranimated.so.tmp
	mv -f hypranimated.so.tmp hypranimated.so

clean:
	rm -rf build hypranimated.so
