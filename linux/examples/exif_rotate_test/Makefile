CXX = g++
CFLAGS = -O2 -Wall -Wextra -D__LINUX__ -I../../../src

all: exif_rotate_test

exif_rotate_test: exif_rotate_test.cpp
	$(CXX) $(CFLAGS) exif_rotate_test.cpp -o exif_rotate_test

# Regenerate every JPEG fixture from scratch (requires ffmpeg + python3).
fixtures:
	python3 make_pattern.py
	python3 make_exif.py base.jpg
	cp base.jpg no_exif.jpg
	chmod u+w *.jpg

clean:
	rm -f exif_rotate_test
