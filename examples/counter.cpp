#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <thread>

int count();
int sum();
int average();
int factorial();

static int _sum = 0;
static double _average = 0;
static double _factorial = 1;

int main()
{
	auto threads = std::array{
		std::thread(sum),
		std::thread(average),
		std::thread(factorial),
		std::thread(count),
	};
	for (auto &thread : threads)
		thread.join();
	std::cout << "_sum = " << _sum << "\n";
	std::cout << "_average = " << _average << "\n";
	std::cout << "_factorial = " << _factorial << "\n";
	assert(_sum == 55);
	assert(_average == 5.5);
	assert(_factorial == 3628800);
	std::cout << "PASS\n";
}

int count()
{
	static int n = 1;

	int fd = open("/dev/kpub/counter", O_WRONLY);
	if (fd < 0) {
		perror("count open");
		return fd;
	}

	while (n <= 10) {
		size_t bytes = write(fd, &n, sizeof(n));
		if (bytes != sizeof(n)) {
			perror("count write");
			close(fd);
			return bytes;
		}
		++n;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	close(fd);

	return 0;
}

int sum()
{
	static int n = 0;

	int fd = open("/dev/kpub/counter", O_RDONLY);
	if (fd < 0) {
		perror("sum open");
		return fd;
	}

	while (true) {
		size_t bytes = read(fd, &n, sizeof(n));
		if (bytes == 0)
			break;
		if (bytes != sizeof(n)) {
			perror("sum read");
			close(fd);
			return bytes;
		}
		_sum += n;
	}

	close(fd);

	return 0;
}

int average()
{
	static int n = 0;
	static int sum = 0;
	static int count = 0;

	int fd = open("/dev/kpub/counter", O_RDONLY);
	if (fd < 0) {
		perror("average open");
		return fd;
	}

	while (true) {
		size_t bytes = read(fd, &n, sizeof(n));
		if (bytes == 0)
			break;
		if (bytes != sizeof(n)) {
			perror("average read");
			close(fd);
			return bytes;
		}
		sum += n;
		++count;
	}

	_average = (double)sum / count;

	close(fd);

	return 0;
}

int factorial()
{
	static int n = 0;

	int fd = open("/dev/kpub/counter", O_RDONLY);
	if (fd < 0) {
		perror("factorial open");
		return fd;
	}

	while (true) {
		size_t bytes = read(fd, &n, sizeof(n));
		if (bytes == 0)
			break;
		if (bytes != sizeof(n)) {
			perror("factorial read");
			close(fd);
			return bytes;
		}
		_factorial *= n;
	}

	close(fd);

	return 0;
}
