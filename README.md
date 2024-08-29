![屏幕截图 2024-08-20 222439](https://github.com/user-attachments/assets/52e39b57-978c-4b1b-ac8e-c28c6e86240e)WebServer
===============
	
本项目基于Linux环境，使用C/C++开发了一个轻量级多线程HTTP服务器。服务器支持多客户端并发连接，提供图片、视频资源访问以及文件上传和删除功能。
主要工作：
◆ 利用Socket来实现客户端和服务器之间的通信；
◆ 利用epoll技术实现I/O多路复用，提高了效率和服务器处理高并发的能力；
◆ 对浏览器的GET和POST请求进行处理，使用有限状态机逻辑高效解析HTTP报文；
◆ 实现了线程池机制，通过多线程技术提供并行服务，提升了服务器的并发处理能力;
◆ 实现了基于双向升序链表的对非活跃连接的定时删除功能，优化了服务器的资源管理。

**效果展示**
> 查看图片
![屏幕截图 2024-08-20 222548](https://github.com/user-attachments/assets/52e778fe-279f-4393-a61e-73d761f8f77f)

> 查看视频
![屏幕截图 2024-08-20 222650](https://github.com/user-attachments/assets/6d2e6fc0-5177-4903-8b0e-a06620332060)

> 文件上传和删除
![屏幕截图 2024-08-20 222439](https://github.com/user-attachments/assets/0db91147-7836-498a-85c5-cab301631128)


