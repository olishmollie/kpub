#include <fcntl.h>
#include <unistd.h>

#include <thread>
#include <cstdio>
#include <iostream>
#include <array>

int count();
int sum();
int average();

int main()
{
	auto threads = std::array{
		std::thread(sum),
		std::thread(average),
		std::thread(count),
	};
	for (auto &thread : threads)
		thread.join();
}

int count()
{
	static int n = 0;

	int fd = open("/dev/kpub/counter", O_WRONLY);
	if (fd < 0) {
		perror("count open");
		return fd;
	}

	while (n < 10) {
		size_t bytes = write(fd, &n, sizeof(n));
		if (bytes != sizeof(n)) {
			perror("count write");
			close(fd);
			return bytes;
		}
		++n;
		std::cout << "n = " << n << "\n";
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	close(fd);

	return 0;
}

int sum()
{
	static int n = 0;
	static int sum = 0;

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
		sum += n;
	}

	std::cout << "sum = " << sum << "\n";

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
		sum += n;
		count++;
	}

	std::cout << "average = " << sum / count << "\n";

	close(fd);

	return 0;
}
