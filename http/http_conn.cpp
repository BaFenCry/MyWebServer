#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>
#include <sstream>
using std::cout,std::cin,std::endl;
//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;


void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        // printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, char *file_storage,int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_file_storage=file_storage;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();

}

string http_conn::get_Files_List()
{
    string tmp;
    for(auto &s:file_list) tmp+=s+"\n";
    return tmp.substr(0,tmp.size()-1);
}

void http_conn::GetAllFilesInDirectory(const std::string &path)
{
    file_list.clear();
    DIR* directory;
    struct dirent* entry;
 
    directory = opendir(path.c_str());
 
    if (directory != nullptr)
    {
        while ((entry = readdir(directory)))
        {
            if (entry->d_type == DT_REG)
            {
                // std::cout << entry->d_name << std::endl;
                file_list.insert(string(entry->d_name));
            }
        }
 
        closedir(directory);
    }
}


//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
    file_name.clear();
    m_file.clear();
    cache.clear();
}

void http_conn::read_buffer_init()
{
    m_checked_idx=0;
    m_read_idx=0;
    m_start_line=0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            } 
            return LINE_BAD; 
        }
    }
    return LINE_OPEN; 
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        int temp=m_read_idx;
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        // printf("%s\n",m_read_buf);
        m_read_idx += bytes_read;
        if (bytes_read <= 0) 
        {
            return false;
        }
        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;

        }
        return true; 
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    file_name2=string(m_url+1);
    // cout<<"file_name2:"<<file_name2<<endl;
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "Content-Type:", 13) == 0)
    {
        text += 13;
        text += strspn(text, " \t");
        // "; boundary="
        if(strncasecmp(text, "multipart/form-data", 19)==0) 
        {
            m_content_type = (char *)malloc(sizeof(char) * 19);
            strncpy(m_content_type,text,19);
            m_content_type[19]='\0';
            text+=(19+11);
            beg_boundary=move(text);
            beg_boundary="--"+beg_boundary;
            end_boundary=beg_boundary+"--";
            // cout<<beg_boundary<<endl;
            // cout<<end_boundary<<endl;
            m_method=PUT;
        }
        text += strspn(text, " \t");
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if(m_method==POST)
    {
        //POST请求中最后为输入的用户名和密码
        text[m_content_length] = '\0';
        m_string = text;
        // printf("m_string:%s",m_string);
        if(strcmp(m_string, "delete")==0)
        {
            file_name2=urlDecode(upload_path+file_name2);
            cout<<file_name2<<endl;
            const char* temp=file_name2.c_str();
            if (std::remove(temp) == 0) {
                std::cout << "文件已被删除。" << std::endl;
                file_list.erase(file_name2.substr(file_name2.rfind('/')+1));
                return DELETE_SUCCESS;
            } else {
                // 删除失败，可能是文件不存在或没有足够的权限
                std::cerr << "删除文件失败。" << std::endl;
                return DELETE_FAILED;
            }
        }
        return GET_REQUEST;
    }
    else if(m_method==PUT)
    {
        m_file=string(text,m_read_idx-m_start_line);
        // cout<<m_file<<endl;
        string content; //消息体内容
        bool end_flag=false;
        auto content_beg=m_file.find(beg_boundary);
        auto content_end=m_file.rfind(end_boundary);
        if(content_beg!=string::npos && content_end!=string::npos)
        {
            if(content_beg==content_end)
            {
                cout<<"未找到初始边界，找到结束边界"<<endl;
                content=m_file.substr(0,content_end-2);
                file_list.insert(file_name); //加入文件目录
                end_flag=true;
                write_file(upload_path+file_name,content);
            }
            else
            {
                cout<<"一次接受全部"<<endl;
                size_t beg,end;
                beg=m_file.find("filename=")+10;
                end=m_file.find("\"",beg);
                file_name=m_file.substr(beg,end-beg);
                content_beg=m_file.find("\r\n\r\n")+4;

                content=m_file.substr(content_beg,content_end-content_beg-2);
                file_list.insert(file_name);
                write_file(upload_path+file_name,content);
                end_flag=true;
            }
        }
        else if(content_beg!=string::npos && content_end==string::npos)
        {
            cout<<"找到初始边界，未找到结束边界"<<endl;
            size_t beg,end;
            beg=m_file.find("filename=")+10;
            end=m_file.find("\"",beg);
            file_name=m_file.substr(beg,end-beg);
            content_beg=m_file.find("\r\n\r\n")+4;
            content=m_file.substr(content_beg);
            write_file(upload_path+file_name,content);
            end_flag=false;
        }
        else
        {
            cout<<"未找到初始边界，未找到结束边界"<<endl;
            //未接收到正确边界，边界被分割,且末尾有可能包含部分边界
            cout<<"m_file size:"<<m_file.size()<<endl;
            cout<<"READ_BUFFER_SIZE:"<<READ_BUFFER_SIZE<<endl;
            if(m_file.size()<100)
            {
                cout<<cache<<endl;
                string tmp=cache+m_file;
                int idx;
                if((idx=tmp.find(end_boundary))!=string::npos)
                {
                    int len=cache.size()-idx+2;
                    truncate_file(upload_path+file_name,len);
                    file_list.insert(file_name);
                    end_flag=true;
                }
                else
                {
                    write_file(upload_path+file_name,m_file);
                    end_flag=false;
                }
            }
            else{ 
                write_file(upload_path+file_name,m_file);
                end_flag=false;
            }
        }
        read_buffer_init();
        if(end_flag) return PUT_REQUEST;
        else 
        {
            if(m_file.size()>100)
                cache=m_file.substr(m_file.size()-100);
            else
            {
                cache+=m_file;
                if(cache.size()>100) 
                    cache=cache.substr(cache.size()-100);
            }
            return NO_FULLY_RECEIVED;
        }
    }
    return NO_REQUEST;
}

std::string http_conn::urlDecode(const std::string& encoded) {
    std::ostringstream decoded;
    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            // 获取 % 后面的两个字符
            std::string hex = encoded.substr(i + 1, 2);
            // 将这两个字符转换为整数值
            int value = std::stoi(hex, nullptr, 16);
            // 将这个整数值转换为字符并添加到结果中
            decoded << static_cast<char>(value);
            i += 2; // 跳过这两个字符
        } else if (encoded[i] == '+') {
            decoded << ' '; // + 号转换为空格
        } else {
            decoded << encoded[i];
        }
    }
    return decoded.str();
}

void http_conn::write_file(string file,string &content){
    std::ofstream ofs(file, std::ios::binary | std::ios::app );
    ofs<<content;
    ofs.flush();
    ofs.close(); 
}

void http_conn::truncate_file(const std::string& filename, std::size_t len) {
    // 打开文件
    int fd = open(filename.c_str(), O_WRONLY);
    if (fd == -1) {
        std::cerr << "Error opening file: " << filename << "\n";
        return;
    }

    // 获取文件大小
    off_t file_size = lseek(fd, 0, SEEK_END);

    // 计算新的文件大小
    if (len > file_size) {
        std::cerr << "Error: len is larger than the file size.\n";
        close(fd);
        return;
    }

    off_t new_size = file_size - len;

    // 截断文件
    if (ftruncate(fd, new_size) == -1) {
        std::cerr << "Error truncating file.\n";
    }

    // 关闭文件
    close(fd);
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            // cout<<ret<<endl;
            if (ret == GET_REQUEST)
            {
                return do_request();
            }
            else
            {
                return ret;
            }
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

bool http_conn::create_directory_if_not_exists(const std::string& dir_path) {
    struct stat info;

    // 检查目录是否存在
    if (stat(dir_path.c_str(), &info) != 0) {
        // 目录不存在，尝试创建
        if (errno == ENOENT) {
            if (mkdir(dir_path.c_str(), 0755) == 0) {
                std::cout << "Directory created successfully.\n";
                return true;
            } else {
                std::cerr << "Failed to create directory: " << strerror(errno) << "\n";
                return false;
            }
        } else {
            // 其他错误
            std::cerr << "Error checking directory: " << strerror(errno) << "\n";
            return false;
        }
    } else if (info.st_mode & S_IFDIR) {
        // 目录已经存在
        std::cout << "Directory already exists.\n";
        return true;
    } else {
        std::cerr << "Path exists but is not a directory.\n";
        return false;
    }
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/');
    //处理cgi
    printf("导航%s\n",p+1);
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);
        //将用户名和密码提取出来
        //user=123&password=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        printf("%s\n%s\n%s\n",m_string,name,password);

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
        string tmp(doc_root);
        upload_path=tmp.substr(0,tmp.rfind('/'))+"/upload_files/";
        upload_path=upload_path+name+"/";
        cout<<upload_path<<endl;
        create_directory_if_not_exists(upload_path);
        GetAllFilesInDirectory(upload_path);
    }
    
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7') 
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
 
        free(m_url_real);
    }
    else
    {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST; 

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else 
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    case PUT_REQUEST:
    {
        add_status_line(200, ok_200_title);
        string tmp=get_Files_List();
        const char *text=tmp.c_str();
        // printf("%s",text);
        add_headers(strlen(text));
        if(!add_content(text))
            return false;
        break;
    }
    case DELETE_SUCCESS:
    {
        add_status_line(200, ok_200_title);
        string tmp=get_Files_List();
        const char *text=tmp.c_str();
        // printf("%s",text);
        add_headers(strlen(text));
        if(!add_content(text))
            return false;
        break;
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    if(read_ret==NO_FULLY_RECEIVED)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    cout<<"read_ret:"<<read_ret<<endl;
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }

    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
