//
// Created by 神圣•凯莎 on 2023/3/12.
//

#ifndef DROGON_HTTP_PRINTER_H
#define DROGON_HTTP_PRINTER_H

#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <iostream>
#include <boost/thread.hpp>

using namespace boost::asio;

class printer
{
  public:
    // 构造函数，初始化定时器对象
    explicit printer(io_service& io, int i = 1)
      : timer_i(i)
      , timer_(io, boost::posix_time::seconds(i))
    {
        // 启动异步等待操作，并绑定回调函数
        timer_.async_wait(boost::bind(&printer::print, this));
    }

    // 回调函数，打印消息并重置定时器
    void print()
    {
        std::cout << hash_value(boost::this_thread::get_id()) << std::endl;
        std::cout << timer_i << std::endl;

        // 重置定时器的过期时间为当前时间加一秒
        timer_.expires_at(timer_.expires_at() + boost::posix_time::seconds(timer_i));

        // 再次启动异步等待操作，并绑定回调函数
        timer_.async_wait(boost::bind(&printer::print, this));
    }

  private:
    int timer_i = 1;
    deadline_timer timer_;  // 定时器对象
};

#endif  //DROGON_HTTP_PRINTER_H
