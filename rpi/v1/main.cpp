//
// File: main.cpp
// Author: Sergey Malahov
// Created: 2025-09-12
//
// Description:
//   Implements simple logging daemon for CtlPie (for raspberry PI)
//   The daemon reads data from the hardware part of CtlPie via IOCTL and writes it into binary file(s) as is
//
//   Separate writer thread:
//   since file write syscall may block for a significant period of time (sometimes), actual file write
//   calls are done in a separate thread.
//   Data is split into chunks and written in separate files to prevent data loss in case of
//   unexpected power down
//
//   Sparse data cooldown:
//   data source (stm32) and receiver (this code) are not synchronized and
//   if the read rate is bigger than the sample rate at stm32, the gaps between samples will be filled with 0x00s
//   The faster we read - the more 0x00 we receive, the larger output file gets.
//   Sparce data detection exists to detect such moments and apply a short colldown to let stm32 to fill its internal buffer
//   with samples

#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#include <iostream>
#include <fstream>
#include <functional>
#include <limits>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <latch>

// SPI device settings
#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_FREQ 6'000'000
#define SPI_BITS 8
#define SPI_DELAY 0

// Receiver software settings
#define SPI_PACKET_SIZE 8
#define XFER_BUFFER_SIZE 1024
#define FILE_CHUNK_SIZE (5 * 1024 * XFER_BUFFER_SIZE)
#define FILE_CHUNK_COUNT 2

#define SPARSE_DATA_CHECK_SIZE (2 * SPI_PACKET_SIZE)
#define SPARSE_DELAY_MS 2

const std::string dirPattern = "data_";
const std::string chunkPattern = "chunk_";

#define ASSERT(b, msg) \
    do { \
        if(!(b)) { \
            throw std::runtime_error(msg); \
        } \
    } while(0)

//TODO: Process SIGTERM correctly, set terminated variable to exit reading/writing loops

uint8_t tx[XFER_BUFFER_SIZE] = {};
uint8_t rx[FILE_CHUNK_COUNT][FILE_CHUNK_SIZE];
static_assert(FILE_CHUNK_SIZE % XFER_BUFFER_SIZE == 0, "");

std::atomic<bool> terminated = false;

int main() {
    int error = 0;

    constexpr auto noIndex = std::numeric_limits<size_t>::max();
    auto writerIndex = noIndex;
    std::mutex mut;
    std::condition_variable cv;
    std::string dstDir;
    std::thread writer;

    try {
        // SPI descriptor guard
        std::unique_ptr<int, decltype([](int* fd) {
            if(*fd >= 0) {
                ::close(*fd);
            }
            delete fd;
        })> fd(new int(-1));

        *fd = ::open(SPI_DEVICE, O_RDWR);
        ASSERT(*fd >= 0, "Failed to open SPI device");

        // Define & create the dst directory for the data
        size_t fileIndex = 0;
        dstDir = dirPattern;
        do{
            dstDir.resize(dirPattern.size());
            dstDir += std::to_string(++fileIndex);
        } while(std::filesystem::exists(dstDir));

        ASSERT(std::filesystem::create_directory(dstDir), "Unable to create directory " + dstDir);

        // Create file writing thread
        std::latch writerLatch{1};
        writer = std::thread([&writerLatch, &writerIndex, &mut, &noIndex, &cv, &dstDir]() {
            size_t fileIndex = 0;
            std::string dstFile = dstDir + "/" + chunkPattern;
            std::unique_lock lock(mut);
            writerLatch.count_down();
            for(;;) {
                cv.wait(lock, [&writerIndex, &noIndex](){
                    return writerIndex != noIndex || terminated;
                });
                if(terminated) {
                    return;
                }
                std::ofstream dst(dstFile + std::to_string(++fileIndex), std::ios::binary);
                dst.write(reinterpret_cast<char*>(rx[writerIndex]), FILE_CHUNK_SIZE);
                writerIndex = noIndex;
                dst.close();
            }
        });

        // Wait the writer thread to prepare
        writerLatch.wait();

        // IOCTL call structure
        spi_ioc_transfer transfer = {
            .tx_buf = (unsigned long)tx,
            .len = sizeof(tx),
            .speed_hz = SPI_FREQ,
            .delay_usecs = SPI_DELAY,
            .bits_per_word = SPI_BITS,
            .cs_change = 0,
            .tx_nbits = 0,
            .rx_nbits = 0,
            .word_delay_usecs = 0,
        };
        size_t byteCount = 0;
        size_t byteCountPrev = 0;
        size_t sparseCount = 0;

        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        while(!terminated) {
            size_t bufferIndex = (byteCount / FILE_CHUNK_SIZE) % FILE_CHUNK_COUNT;
            transfer.rx_buf = reinterpret_cast<unsigned long>(&rx[bufferIndex][byteCount % FILE_CHUNK_SIZE]);
            int res = ::ioctl(*fd, SPI_IOC_MESSAGE(1), &transfer);
            ASSERT(res > 0, "Failed to send SPI data");

            // Sparse data cooldown
            if(SPARSE_DELAY_MS) {
                bool sparseData = true;
                for(size_t i = 0; i < SPARSE_DATA_CHECK_SIZE; ++i) {
                    if(rx[bufferIndex][byteCount % FILE_CHUNK_SIZE + XFER_BUFFER_SIZE - 1 - i] != 0) {
                        sparseData = false;
                        break;
                    }
                }
                if(sparseData) {
                    ++sparseCount;
                    std::this_thread::sleep_for(std::chrono::milliseconds(SPARSE_DELAY_MS));
                }
            }
            byteCount += sizeof(tx);

            // Another chunk of data received - trigger file write operation in the writer's thread
            if(byteCount % FILE_CHUNK_SIZE == 0) {
                // Pass the received chunk to the writer
                std::unique_lock lock(mut);
                writerIndex = bufferIndex;
                lock.unlock();
                cv.notify_all();
            }

            // stdout some statistics
            if(byteCount % (500 * XFER_BUFFER_SIZE) == 0) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
                std::cout << std::setprecision(3)
                    << "Stat: "
                    << ((1000 * (byteCount - byteCountPrev)/ 1024) / ms) << " KB/s, "
                    << (1.0 * byteCount / (1024 * 1024)) << " MB, "
                    << (1000 * SPARSE_DELAY_MS * sparseCount / ms) << " Sparse ms/s "
                    << "         "
                    << "\r"
                    << std::flush;
                byteCountPrev = byteCount;
                sparseCount = 0;
                start = clock::now();
            }
        }
    } catch(const std::runtime_error& e) {
        std::cerr << "Failed: " << e.what() << std::endl;
        error = 1;
    }

    // Release the writer
    {
        std::unique_lock lock(mut);
        terminated = true;
        cv.notify_all();
    }

    if(writer.joinable()) {
        writer.join();
    }

    return error;
}
