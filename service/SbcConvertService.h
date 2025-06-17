//
// Created by 天使之王·彦 on 2021/11/13.
//

#ifndef LEARNING_CPP_SBCCONVERTSERVICE_H
#define LEARNING_CPP_SBCCONVERTSERVICE_H

#include "string"

class SbcConvertService
{
  public:
    static std::wstring s2ws(const std::string& str);

    static std::string ws2s(const std::wstring& wstring);

    static std::wstring wsQj2bj(const std::wstring& src);

    static int charConvert(const wchar_t& src);

    static int qj2bj(const wchar_t& src);
};

#endif  //LEARNING_CPP_SBCCONVERTSERVICE_H
