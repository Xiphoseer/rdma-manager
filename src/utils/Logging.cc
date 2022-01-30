#include "Logging.h"

#define ESC_NORMAL "\e[0m"
#define ESC_DIM "\e[2m"
#define ESC_UNSET "\e[39m"
#define ESC_RED "\e[91m"
#define ESC_BLUE "\e[94m"
#define ESC_YELLOW "\e[93m"

namespace rdma
{
    std::string TAGS[6] = {
        "",
        "[DEBUG]: ",
        ESC_BLUE "[INFO]: " ESC_UNSET,
        ESC_YELLOW "[WARN]: " ESC_UNSET,
        ESC_RED "[ERROR]: " ESC_UNSET,
        ESC_RED "[FATAL]: " ESC_UNSET,
    };

    Logging *Logging::logger = new Logging();

    Logging::~Logging() {}

    void Logging::log(int level, string msg)
    {
        std::cerr << TAGS[level] << msg << endl;
    }

    void Logging::log(int level, string filename, int line, string msg)
    {
        std::cerr << TAGS[level] << msg << endl
                  << " at " ESC_DIM << filename << ":" << line << ESC_NORMAL << endl;
    }
}