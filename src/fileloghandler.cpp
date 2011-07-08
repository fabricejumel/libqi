/**
 * fileloghandler.cpp
 * Author(s):
 *  - Herve Cuche <hcuche@aldebaran-robotics.com>
 */

#include <qi/log/fileloghandler.hpp>

#include <boost/function.hpp>
#include <boost/filesystem.hpp>
#include <boost/bind.hpp>
#include <string>
#include <qi/log.hpp>
#include <qi/os.hpp>
#include <cstdio>

#define CATSIZEMAX 16

namespace qi {
  namespace log {
    FileLogHandler::FileLogHandler(const std::string& filePath)
    {
      fFile = NULL;
      boost::filesystem::path fPath(filePath);
      // Create the directory!
      try
      {
        if (!boost::filesystem::exists(fPath.make_preferred().parent_path()))
          boost::filesystem::create_directories(fPath.make_preferred().parent_path());
      }
      catch (boost::filesystem::filesystem_error &e)
      {
        qiLogWarning("qi.log.fileloghandler") << e.what() << std::endl;
      }
      // Open the file.
      FILE* file = qi::os::fopen(fPath.make_preferred().string().c_str(), "w+");

      if(file)
        fFile = file;
      else
        qiLogWarning("qi.log.fileloghandler") << "Cannot open " << filePath << std::endl;
    }


    FileLogHandler::FileLogHandler(const FileLogHandler &rhs)
      : fFile(new FILE)
    {
      *fFile = *rhs.fFile;
    }

    const FileLogHandler & FileLogHandler::operator=(const FileLogHandler &rhs)
    {
      *fFile = *rhs.fFile;
      return *this;
    }

    FileLogHandler::~FileLogHandler()
    {
      if (fFile != NULL)
        fclose(fFile);
    }

    void FileLogHandler::cutCat(const char* category, char* res)
    {
      int categorySize = strlen(category);
      if (categorySize < CATSIZEMAX)
      {
        memset(res, ' ', CATSIZEMAX);
        memcpy(res, category, strlen(category));
      }
      else
      {
        memset(res, '.', CATSIZEMAX);
        memcpy(res + 3, category + categorySize - CATSIZEMAX + 3, CATSIZEMAX - 3);
      }
      res[CATSIZEMAX] = '\0';
    }


    void FileLogHandler::log(const qi::log::LogLevel verb,
                             const char              *category,
                             const char              *msg,
                             const char              *file,
                             const char              *fct,
                             const int               line)
    {
      if (verb > qi::log::getVerbosity() || fFile == NULL)
      {
        return;
      }
      else
      {
        const char* head = logLevelToString(verb);
        char fixedCategory[CATSIZEMAX + 1];
        fixedCategory[CATSIZEMAX] = '\0';
        cutCat(category, fixedCategory);
        if (qi::log::getContext())
        {
          fprintf(fFile, "%s %s: %s(%d) %s %s", head, fixedCategory, file, line, fct, msg);
        }
        else
        {
          fprintf(fFile,"%s %s: %s", head, fixedCategory, msg);
        }
        fflush(fFile);
      }
    }
  }
}
