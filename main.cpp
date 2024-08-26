#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

#include "clipper.hpp"

enum ErrorType : int
{
    USAGE_ERROR = 1,
    FILE_ERROR,
    RUNTIME_ERROR
};

int main(int argc, char* argv[])
{
    static const char* usage =
        "Usage:\n"
        "\txclipp STRING\n"
        "\txclipp -f FILE\n";

    bool is_file = false;
    char* str = nullptr;
    int opt = 0;
    while ((opt = getopt(argc, argv, "-f")) != -1)
    {
        switch (opt)
        {
            case 1:
            {
                if (str != nullptr)
                {
                    std::fputs(usage, stderr);
                    return USAGE_ERROR;
                }
                str = argv[optind - 1];
                break;
            }
            case 'f':
            {
                is_file = true;
                break;
            }
            default:
            {
                std::fputs(usage, stderr);
                return USAGE_ERROR;
            }
        }
    }

    if (str == nullptr)
    {
        std::fputs(usage, stderr);
        return USAGE_ERROR;
    }

    std::string_view data;
    char* file_name = nullptr;
    if (is_file)
    {
        file_name = realpath(str, NULL);
        if (file_name == nullptr)
        {
            std::perror("realpath");
            return FILE_ERROR;
        }
        data = file_name;
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
        std::free(file_name);
        return RUNTIME_ERROR;
    }

    std::free(file_name);

    return 0;
}

