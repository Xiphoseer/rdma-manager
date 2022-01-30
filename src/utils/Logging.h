/**
 * @file Logging.h
 * @author cbinnig, lthostrup, tziegler
 * @date 2018-08-17
 */

#ifndef LOGGING_HPP_
#define LOGGING_HPP_

#include "./Config.h"

#include <iostream>
#include <string>

namespace rdma
{

  class Logging
  {
  public:
    static Logging *logger;
    static void debug(string filename, int line, string msg)
    {
      // avoid unused variable warning
      (void)filename;
      (void)line;
      (void)msg;
#ifdef DEBUG
      if (Config::LOGGING_LEVEL <= 1)
        logger->log(1, filename, line, msg);
#endif
    }

    static void error(string filename, int line, string msg)
    {
      if (Config::LOGGING_LEVEL <= 4)
        logger->log(4, filename, line, msg);
    }

    static void errorNo(string filename, int line, char *errorMsg, int errNo)
    {
      if (Config::LOGGING_LEVEL <= 4)
      {
        std::stringstream msg;
        msg << "E" << errNo << ": " << errorMsg;
        logger->log(4, filename, line, std::move(msg.str()));
      }
    }

    static void fatal(string filename, int line, string msg)
    {
      if (Config::LOGGING_LEVEL <= 5)
        logger->log(5, filename, line, msg);
      exit(1);
    }

    static void info(string msg)
    {
      if (Config::LOGGING_LEVEL <= 2)
        logger->log(2, msg);
    }

    static void warn(string msg)
    {
      if (Config::LOGGING_LEVEL <= 3)
        logger->log(3, msg);
    }

    virtual ~Logging();
  protected:
    virtual void log(int level, string filename, int line, string msg);
    virtual void log(int level, string msg);
  };

} // end namespace rdma

#endif /* LOGGING_HPP_ */
