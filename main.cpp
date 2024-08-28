#include <cstdio>
#include <cstdlib>
#include <memory>
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

static const char* usage =
        "Usage:\n"
        "\txclipp STRING\n"
        "\txclipp -f [--] FILE\n";

int main(int argc, char* argv[])
{
    bool is_file = false;
    char* str = nullptr;

    if (argc == 2)
    {
        str = argv[1];
    }
    else
    {
        int opt = 0;
        while ((opt = getopt(argc, argv, "f")) != -1)
        {
            switch (opt)
            {
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
        if (optind == argc)
        {
            std::fputs("No STRING or FILE was provided\n", stderr);
            std::fputs(usage, stderr);
            return USAGE_ERROR;
        }
        str = argv[optind];
    }

    std::string_view data;

    std::unique_ptr<char, decltype(&std::free)> file_name{nullptr, std::free};
    if (is_file)
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

