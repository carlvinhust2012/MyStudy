epoll一般和多线程配合使用，作为服务端，每一个线程对应一个连接，IO事件一般会造成阻塞。

如何使用epoll呢？
1.先nEpollfd=epoll_create(MAX_EVENT);生成一个文件句柄nEpollfd;
2.再创建一个socket，得到socket句柄 nListenfd = socket(AF_INET, SOCK_STREAM, 0);
3.将socket设置为非阻塞setNonBlocking(nListenfd)
int setNonBlocking(int p_nSock)
{   
    int nOpts;   
    nOpts=fcntl(p_nSock,F_GETFL); // 调用文件控制函数，获取当前socket文件句柄的属性，F_GETFL-->get flag  
    if(nOpts<0)   
    {   
        printf("[%s %d] Fcntl Sock GETFL fail!\n",__FUNCTION__,__LINE__);
        return -1;
    }   

    nOpts = nOpts|O_NONBLOCK;   
    if(fcntl(p_nSock,F_SETFL,nOpts)<0) // 调用文件控制函数，设置当前socket文件句柄的属性，再参数中添加非阻塞  
    {  
        printf("[%s %d] Fcntl Sock SETFL fail!\n",__FUNCTION__,__LINE__);
        return -1;   
    } 

    return 0;
}   

4.给socket文件句柄绑定IP+port，bind(nListenfd,(struct sockaddr *)&serveraddr, sizeof(serveraddr));
5.监听这个socket句柄，listen(nListenfd,MAX_LISTENQ)；
6.将socket句柄加入到已经创建好的epoll实例，告诉内核要监听哪一类事件；ev.data.fd=nListenfd;  ev.events=EPOLLIN;//socket读事件，默认使用EPOLLLT模式    
  epoll_ctl(nEpollfd,EPOLL_CTL_ADD,nListenfd,&ev)
7.循环遍历内核送上来的事件列表events，最大不超过创建epoll实例的个数；
for (;;)  {   
        nEventNum = epoll_wait(nEpollfd, events, MAX_EVENT, -1);
}

epoll有3个系统调用：
epoll_create()、epoll_ctl()、epoll_create()
(1) 创建一个epoll句柄，size用来告诉内核这个监听的数目一共有多大
int epoll_create(int size); 
当创建好epoll句柄后，它就是会占用一个fd值，在linux下如果查看/proc/进程id/fd/，是能够看到这个fd的，所以在使用完epoll后，必须调用close()关闭，否则可能导致fd被耗尽。

(2) epoll的事件注册函数，先注册要监听的事件类型，它不同于select()是在监听事件时告诉内核要监听什么类型的事件
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event); 
int epfd----第一个参数是epoll_create()的返回值，表示一个epoll文件描述符（句柄）
int op  ----第二个参数表示动作，用三个宏来表示：
            EPOLL_CTL_ADD：注册新的fd到epfd中；
            EPOLL_CTL_MOD：修改已经注册的fd的监听事件；
            EPOLL_CTL_DEL：从epfd中删除一个fd；

int fd----第三个参数是需要监听的fd。
struct epoll_event *event-----第四个参数是告诉内核需要监听什么事，struct epoll_event结构如下：

//保存触发事件的某个文件描述符相关的数据（与具体使用方式有关）  
typedef union epoll_data {  
    void *ptr;  
    int fd; //这里的fd是我们创建一个socket后，得到的socket文件描述符  
    __uint32_t u32;  
    __uint64_t u64;  
} epoll_data_t; 

 //感兴趣的事件和被触发的事件  
struct epoll_event {  
    __uint32_t events; /* Epoll events */  
    epoll_data_t data; /* User data variable */  
}; 

events可以是以下几个宏的集合：
EPOLLIN ：表示对应的文件描述符可以读（包括对端SOCKET正常关闭）；
EPOLLOUT：表示对应的文件描述符可以写；
EPOLLPRI：表示对应的文件描述符有紧急的数据可读（这里应该表示有带外数据到来）；
EPOLLERR：表示对应的文件描述符发生错误；
EPOLLHUP：表示对应的文件描述符被挂断；
EPOLLET： 将EPOLL设为边缘触发(Edge Triggered)模式，这是相对于水平触发(Level Triggered)来说的。
EPOLLONESHOT：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里。

(3) 收集在epoll监控的事件中已经发送的事件
int epoll_wait(int epfd, struct epoll_event * events, int maxevents, int timeout);
int epfd----第一个参数是epoll_create()的返回值，表示一个epoll文件描述符（句柄）
参数events是分配好的epoll_event结构体数组，epoll将会把发生的事件赋值到events数组中（events不可以是空指针，内核只负责把数据复制到这个events数组中，不会去帮助我们在用户态中分配内存）。
maxevents告之内核这个events有多大，这个 maxevents的值不能大于创建epoll_create()时的size，参数timeout是超时时间（毫秒，0会立即返回，-1将不确定，也有说法说是永久阻塞）。
如果函数调用成功，返回对应I/O上已准备好的文件描述符数目，如返回0表示已超时。

epoll有水平触发（LT）和边沿触发（ET）
