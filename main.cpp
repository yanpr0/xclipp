#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "clipper.hpp"

enum ErrorType : int
{
    USAGE_ERROR = 1,
    FILE_ERROR,
    RUNTIME_ERROR
};

static const char* usage =
        "Usage:\n"
        "\txclipp [--] STRING\n"
        "\txclipp -f [--] FILE\n"
        "\txclipp -c [--] FILE\n";

class MmapDeleter
{
public:
    explicit MmapDeleter(std::size_t size = 0) : size_{size}
    {
    }

    void operator()(char* ptr)
    {
        if (size_ <= static_cast<std::size_t>(sysconf(_SC_PAGESIZE)))
        {
            delete[] ptr;
        }
        else
        {
            munmap(ptr, size_);
        }
    }

private:
    std::size_t size_;
};

int main(int argc, char* argv[])
{
    bool is_content = false;
    bool is_file = false;
    char* str = nullptr;

    if (argc == 2)
    {
        str = argv[1];
    }
    else
    {
        int opt = 0;
        while ((opt = getopt(argc, argv, "fc")) != -1)
        {
            switch (opt)
            {
                case 'f':
                {
                    is_file = true;
                    break;
                }
                case 'c':
                {
                    is_content = true;
                    break;
                }
                default:
                {
                    std::fputs(usage, stderr);
                    return USAGE_ERROR;
                }
            }
        }
        if (optind == argc)
        {
            std::fputs("No STRING or FILE was provided\n", stderr);
            std::fputs(usage, stderr);
            return USAGE_ERROR;
        }
        if (is_file && is_content)
        {
            std::fputs("Conflicting options were provided\n", stderr);
            std::fputs(usage, stderr);
            return USAGE_ERROR;
        }
        str = argv[optind];
    }

    std::string_view data;

    std::unique_ptr<char, decltype(&std::free)> file_name{nullptr, std::free};
    std::unique_ptr<char, MmapDeleter> file_content;
    if (is_content)
    {
        int fd = open(str, O_RDONLY);
        if (fd == -1)
        {
            std::perror(str);
            return FILE_ERROR;
        }
        struct stat st;
        if (fstat(fd, &st) == -1)
        {
            std::perror(str);
            return FILE_ERROR;
        }
        std::size_t size = st.st_size;
        file_content = {nullptr, MmapDeleter{size}};
        if (size <= static_cast<std::size_t>(sysconf(_SC_PAGESIZE)))
        {
            file_content.reset(new char[size]);
            if (read(fd, file_content.get(), size) == -1)
            {
                std::perror(str);
                return FILE_ERROR;
            }
        }
        else
        {
            void* ptr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (ptr == MAP_FAILED)
            {
                std::perror(str);
                return FILE_ERROR;
            }
            file_content.reset(static_cast<char*>(ptr));
        }
        data = {file_content.get(), size};
    }
    else if (is_file)
    {
        file_name.reset(realpath(str, nullptr));
        if (file_name == nullptr)
        {
            std::perror(str);
            return FILE_ERROR;
        }
        data = file_name.get();
    }
    else
    {
        data = str;
    }

    try
    {
        xcpp::Clipper clipper(data, is_file);
        clipper.Run();
    }
    catch (std::exception& e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return RUNTIME_ERROR;
    }

    return 0;
}

