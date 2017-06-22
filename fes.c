#include "101_PublicAPI.h"
#include "Iprotocol.h"
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <assert.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/timerfd.h>
#include <fcntl.h>
#include<sys/ioctl.h>




//configurable
#define MAX_DEV_NUM 256
#define FESIP "112.123.239.223"//前置的IP地址
#define FESPORT "10720"//与设备通信的端口
#define WWWPORT 8000//与图形通信的端口


#define RESOURCENAME  "/run/fes/RESOURCE_"FESPORT//打印设备登录信息
#define LISTNAME  "/run/fes/LEGAL_DEV_"FESPORT//打印所有设备信息
#define TAGNAME "/run/fes/"VER//VER在makefile中传递过来，表示当前软件版本
#define NOVERSION "/run/fes/NOVERSIONCONTROL"//没有版本控制



typedef struct s_com{
    int fd;
    char ipstr[INET6_ADDRSTRLEN];
    int port;
    BALANCE101VARIOUSFLAG * flag;
    s_dev_info DevInfo;
} s_com;

s_com g_map[MAX_DEV_NUM]={[0 ... MAX_DEV_NUM-1] = {-1,{},-1,NULL,{0,0,0,0,0}}};//gcc specific grammar

int HMI_fd = -1;

S_DEV_INFO *g_pdev=NULL; //got from HMI


char Iprot_buf[2*MAXBUFFERSIZE]={};

//Resources are splitted into three categories

// *Avail  fd == -1, no connection at all
// *Idle   fd != -1, if_online == 0, 
//         connected, does not go through login process
// *log on fd!=-1, if_online == 1,
//         connected, gone through login process,
//         qualified devices


static int ResourceAvail(const int idx);//当前数组是否未被占用
static int ResourceIdle(const int idx);////已连接但还不在线的设备
static int ResourceLogon(const int idx);//设备是否已经登录

static int PrintResource(int idx);//更新当前数组信息到调试文件中的对应行
static int PrintLegalDev();//打印获取到的所有设备ID

static void ReleaseResource(const int idx);//idle or log on =>available//断开与设备的连接并释放数组资源清空设备信息
static int FindResource(const int fd);   //available => idle//查找可用的数组序列，并填充socket id
static int LoginResource(const int idx, char *buf, int len);  //idle =>log on 有效设备，填充设备数据




static int relayData(int idx);  //move data from device socket to HMI socket

static int initHMISock();////创建与图形的socket连接
static void initIProtocol();//初始化私有协议

static int send_HMI_Iprot(const int if_just_login,s_com *pCom, const int data_type,const _YC yc, const _YX yx,const  _SOE soe,const int reason);

static void SENDOUT(int sock_fd,char *buf, int len);//send buf to socket with a loop to ensure everything is sent out向fd写数据
static int ReadUntil(const int fd, const char c, const int timeout,char *packet, const int size ); //timeout in second//读取图形发过来的数据
static void RemovePrintFiles();//删除/run/fes下的调试文件

static void TagVersion();//创建文件/run/fes/gitversion
static void InitDevFlag();//程序启动，统一申请内存，保存flag


int main(int argc, char **argv){
    openlog("fes_"FESPORT , LOG_CONS|LOG_PID|LOG_PERROR, LOG_FTP);
    TagVersion();//创建文件/run/fes/gitversion
    InitDevFlag();//程序启动，统一申请内存，保存flag




    fd_set readfds;
    //fd_set writefds;
    int opt = 1;
    /*
       for (int a = 0; a<256; a++){
       g_map[a].fd = -1;
       g_map[a].flag = NULL;
       g_map[a].if_log = 0;
       }
     */

    int port = 11111;//与设备通信端口
    if (argc > 1){
        port = atoi(argv[1]);
    }
    syslog(LOG_DEBUG, "server started!");

    //host service to devices
    syslog(LOG_INFO, "host the service for devices on port:%d",port);
    char IPADDR_S[16] = "0.0.0.0";//与设备通信地址
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(IPADDR_S);
    server_addr.sin_port = htons(port);

    int server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (server_fd == -1){
        syslog(LOG_ERR, "get socket failed:%s. Exit", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret == -1){
        syslog(LOG_ERR, "setsockopt failed:%s.Exit", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /*
       int flags = fcntl(server_fd, F_GETFL, 0);
       fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
     */

    ret = bind(server_fd, (struct sockaddr*) &server_addr, sizeof(server_addr));
    if (ret == -1) {
        syslog(LOG_ERR, "bind failed:%s.Exit", strerror(errno));
        exit(EXIT_FAILURE);
    }
    ret = listen(server_fd, 5);
    if (ret == -1) {
        syslog(LOG_ERR, "listen failed:%s.Exit", strerror(errno));
        exit(EXIT_FAILURE);
    }

    //FD_ZERO(&readfds);
    //FD_ZERO(&writefds);
    //FD_SET(server_fd, &readfds);


    //timer fd
#define INTV_SEC 1//定时器时间
    syslog(LOG_INFO, "create timer fd with timeout interval %d second",INTV_SEC);
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == -1)
        perror("clock_gettime");

    struct itimerspec new_value;
    new_value.it_value.tv_sec = now.tv_sec + 1;
    new_value.it_value.tv_nsec = now.tv_nsec;
    new_value.it_interval.tv_sec = INTV_SEC;
    new_value.it_interval.tv_nsec = 0;

    int timefd = timerfd_create(CLOCK_REALTIME, 0);
    if (timefd == -1)
        perror("timerfd_create");

    if (timerfd_settime(timefd, TFD_TIMER_ABSTIME, &new_value, NULL) == -1)
        perror("timerfd_settime");

    //FD_ZERO(&readfds);
    //FD_SET(timefd, &readfds);

    // HMI socket
    HMI_fd = initHMISock();////创建与图形的socket连接
    initIProtocol();//初始化私有协议
    //FD_SET(HMI_fd, &readfds);
    //FD_SET(HMI_fd, &writefds);




    int SockFd = 0;
    int result, nread;
    int len;
    while (1) {
        if(HMI_fd == -1)
            HMI_fd = initHMISock();////创建与图形的socket连接

        FD_ZERO(&readfds);
        int MAX = 0;
        FD_SET(timefd, &readfds);
        if(timefd > MAX) MAX = timefd;

        FD_SET(server_fd, &readfds);
        if(server_fd > MAX) MAX = server_fd;

        FD_SET(HMI_fd, &readfds);
        if(HMI_fd > MAX) MAX = HMI_fd;

        for (int a=0; a<MAX_DEV_NUM; a++){
            if(!ResourceAvail(a) ){ //idle or log on//当前数组是否未被占用
                FD_SET(g_map[a].fd, &readfds);
                if(g_map[a].fd > MAX) MAX = g_map[a].fd;
            }
        }

        //printf("select waiting\n");
        result = select(MAX+1, &readfds, NULL, NULL, NULL);
        //printf("select returns\n");
        if (result == -1){
            syslog(LOG_ERR,"select error:%s!",strerror(errno));
            exit(EXIT_FAILURE);
        }
        else if (result == 0){
            syslog(LOG_ERR,"select timeout but should not as no timeout is set!");
            continue;
        }
        else{
            //beat--------------
            if (FD_ISSET(timefd, &readfds)){
                uint64_t exp;
                int s = read(timefd, &exp, sizeof(uint64_t));
                if (s != sizeof(uint64_t)){
                    syslog(LOG_ERR,"read timer error!");
                    exit(EXIT_FAILURE);
                }                

                for (int a = 0; a<MAX_DEV_NUM; a++){
                    if (ResourceLogon(a)){//设备是否已经登录
                        unsigned char buf[MAX101FRAMELEN];
                        len = device_101act_beat(g_map[a].flag, buf, MAX101FRAMELEN);                                   
                        SENDOUT(g_map[a].fd,buf,len);//发数据到设备
                    }
                }
            }
            //continue;
        }

        //add client--------------
        if (FD_ISSET(server_fd, &readfds)){
            if ((SockFd = accept(server_fd, NULL, NULL)) == -1) {
                syslog(LOG_ERR, "accept:%s", strerror(errno));
                //continue;
            }
            else{
                //FD_SET(SockFd, &readfds);

                int a = FindResource(SockFd);//查找可用的数组序列，并填充socket id
                if(a == MAX_DEV_NUM){
                    close(SockFd);
                    syslog(LOG_INFO, "NO more service due to resource limit, the new connection is closed!");

                    for (int a = 0; a<MAX_DEV_NUM; a++){
                        if (ResourceIdle(a)){////已连接但还不在线的设备
                            ReleaseResource(a);//断开与设备的连接并释放数组资源清空设备信息
                            syslog(LOG_INFO, "Idle resource is released!idx=%d",a);
                        }
                    }
                }
                else{
                    syslog(LOG_DEBUG,"a new connection is established, the fd =%d,idx =%d",SockFd,a);
                }
            }
            //continue;
        }


        //see if HMI socket is writeable
        /*
           if(!FD_ISSET(HMI_fd, &writefds)){
           continue; 
           }
         */




        //get data from device
        for (int a = 0; a<MAX_DEV_NUM; a++){
            if ((-1 != g_map[a].fd) && FD_ISSET(g_map[a].fd, &readfds)){                            
                ioctl(g_map[a].fd, FIONREAD, &nread);
                if (nread == 0) {
                    syslog(LOG_INFO,"connection broken, rm client on idx %d,fd %d", a,g_map[a].fd);

                    char buf[MAXBUFFERSIZE]={};
                    g_map[a].DevInfo.if_online = 0;//将设备置为离线状态
                    if(Iprot_dev_login(&(g_map[a].DevInfo),buf,MAXBUFFERSIZE) == 0)//私有协议组帧，离线type=4
                        SENDOUT(HMI_fd,buf,strlen(buf));//将离线信息发送到图形
                    ReleaseResource(a);//断开与设备的连接并释放数组资源清空设备信息

                    //FD_CLR(g_map[a].fd, &readfds);
                }
                else{
                    syslog(LOG_DEBUG, "idx=%d,fd=%d has something to say.",a,g_map[a].fd);
                    int ret = relayData(a);//发送数据到图形
                    if(ret != 0) {//发送数据失败
                        syslog(LOG_INFO,"relay data failure,rm client on fd %d", g_map[a].fd);
                        char buf[MAXBUFFERSIZE]={};
                        g_map[a].DevInfo.if_online = 0;
                        if(Iprot_dev_login(&(g_map[a].DevInfo),buf,MAXBUFFERSIZE) == 0)//组帧后的数据存放到buf
                            SENDOUT(HMI_fd,buf,strlen(buf));
                        ReleaseResource(a);//断开与设备的连接并释放数组资源清空设备信息
                        //FD_CLR(g_map[a].fd, &readfds);
                    }
                }
                //break;
            }
        }


        //get data from HMI
        if(FD_ISSET(HMI_fd, &readfds)){
            ioctl(HMI_fd, FIONREAD, &nread);
            if(nread == 0){
                syslog(LOG_ERR, "HMI connection is broken, exit");
                close(HMI_fd);
                HMI_fd = -1;
                exit(EXIT_FAILURE);
            }
            else{
                char buf[MAXBUFFERSIZE]={};
                int rtn = read(HMI_fd,buf,MAXBUFFERSIZE);
                //rtn == 0 is handled by ioctl
                if(rtn < 0){
                    syslog(LOG_ERR, "HMI connection is broken with error %s, exit",strerror(errno));
                    exit(EXIT_FAILURE);
                }
                else{

                    strncpy(Iprot_buf+strlen(Iprot_buf),buf,strlen(buf));
                    syslog(LOG_DEBUG, "buf to be parsed:%s",Iprot_buf);
                    int type = Protocol_Packet_Recv(Iprot_buf, rtn);  //not complete packet will be left in the Iprot_buf//解析图形数据
                    if(type < 0){
                        syslog(LOG_WARNING, "parse error!"); 
                    }
                    //only handle CMD_DEVINFO
                    else if (type & 1 << CMD_DEVINFO || type & 1 << CMD_DEVINFO_ALIAS){
                        syslog(LOG_INFO,"got new dev info");
                        int devnum = Iprot_getDevInfo(&g_pdev);  //获取设备信息，返回设备总数
                        if(devnum <= 0 || g_pdev == NULL){
                            syslog(LOG_WARNING,"no dev info is got");
                            exit(EXIT_FAILURE);
                        }
                        else{
                            if(devnum > MAX_DEV_NUM){
                                syslog(LOG_ERR,"too many devices(%d) from HMI.",devnum);
                                exit(EXIT_FAILURE);
                            }
                        }
                    }
                }
            }
        }//select

    }//while
}
int relayData(const int idx){
    s_com *pCom = &g_map[idx];
    unsigned char buf[MAX101FRAMELEN]={};
    int fd = pCom->fd;
    //int *if_log = &(pCom->DevInfo.if_online);

    int reason=0;
    int if_just_login = 0;
    _YX yx;
    _YC yc;
    _SOE soe;
    int data_type = 0;

    ssize_t ret = read(fd, buf, MAX101FRAMELEN);
    if (ret <= 0){
        syslog(LOG_INFO,"read fd %d error: %s",fd, strerror(errno));
        return -1;
        //exit(EXIT_FAILURE);
    }


    if (ResourceIdle(idx)){////已连接但还不在线的设备
        /*
           if( pCom->flag!= NULL){
           syslog(LOG_ERR, "if_log ==0 and flag != NULL, error!");
           return -1;
           }
         */
        if (LoginResource(idx,buf,ret) == 0){//有效设备，填充设备数据
            if_just_login = 1;
            syslog(LOG_DEBUG,"adding client on fd %d on idx %d", fd,idx);
        }
        else{
            //unsigned int dev_id = 0;
            //device_get_id(pCom->flag,&dev_id);
            //syslog(LOG_INFO,"the device(%u) login failed!",dev_id);
            return -1;
        }   
    }
    else{
        data_type = device_parse_101(pCom->flag, buf, ret);
        if (data_type & YX_TYPE){
            getYX(pCom->flag, &yx, &reason);
            /* 
               printf("get %d yx\n", yx.Yx_Num);
               for (unsigned int i = 0; i < yx.Yx_Num; i++){
               printf("------------------");
               printf("addr=%d\n", yx.Yx_Data[i].addr);
               printf("state=%d\n", yx.Yx_Data[i].State);
               printf("BL=%d\n", yx.Yx_Data[i].BL);
               printf("SB=%d\n", yx.Yx_Data[i].SB);
               printf("NT=%d\n", yx.Yx_Data[i].NT);
               printf("IV=%d\n", yx.Yx_Data[i].IV);
               }
             */
        }

        if (data_type & YC_TYPE){
            getYC(pCom->flag, &yc, &reason);
        }

        if (data_type & SOE_TYPE){
            getSOE(pCom->flag, &soe, &reason);
            //printf("soe addr 0x%x\n",soe.SOE_[0].addr);
            //printf("soe addr 0x%x\n",soe.SOE_[0].Channel);
        }

        /*

           if (data_type & YK_TYPE){
           _YK yk;
           getYK(flag, &yk);
           }

           if (data_type & CALL_TYPE){
           S_CALL call;
           getCALL(flag, &call);
           }

           if (data_type & FILEMENU_TYPE){
           FILESTR menu;
           getFILEMENU(flag, &menu);
           }

           if (data_type & FILE_TYPE){
           FILESTRUP file;
           getFILE(flag, &file);
           }
         */
    }
    //relay data
    if(data_type & YC_TYPE || data_type & SOE_TYPE || data_type & YX_TYPE ||if_just_login){
        if(send_HMI_Iprot(if_just_login, pCom, data_type, yc,  yx,  soe,reason) != 0)
            syslog(LOG_WARNING, "data relay failed");
    }       
    return 0;
}




int send_HMI_Iprot(const int if_just_login,s_com *pCom, const int data_type,const _YC yc, const _YX yx,const  _SOE soe,const int reason){

    char buf[MAXBUFFERSIZE];

    if(if_just_login){
        syslog(LOG_INFO, "sent to HMI with device login");

        if(Iprot_dev_login(&pCom->DevInfo,buf,MAXBUFFERSIZE) == 0)
            SENDOUT(HMI_fd,buf,strlen(buf));
    }
    if(data_type & YC_TYPE){
        syslog(LOG_INFO, "sent to HMI with YC");
        syslog(LOG_INFO, "reason is %d",reason);
        if(Iprot_yc(&pCom->DevInfo,&yc,buf,MAXBUFFERSIZE) == 0)
            SENDOUT(HMI_fd,buf,strlen(buf));

    }
    if(data_type & YX_TYPE){
        syslog(LOG_INFO, "sent to HMI with YX");
        syslog(LOG_INFO, "reason is %d",reason);
        if(Iprot_yx(&pCom->DevInfo,&yx,buf,MAXBUFFERSIZE) == 0)
            SENDOUT(HMI_fd,buf,strlen(buf));
    }
    if(data_type & SOE_TYPE){
        syslog(LOG_INFO, "sent to HMI with SOE");
        if(Iprot_soe(&pCom->DevInfo,&soe,buf,MAXBUFFERSIZE) == 0)
            SENDOUT(HMI_fd,buf,strlen(buf));
    }
    return 0;
}

//当前数组是否未被占用
inline int ResourceAvail(const int idx){
    if (-1 != g_map[idx].fd) //connected
        return 0;
    else
        return 1; //OK to use
}

//设备是否已经登录
inline int ResourceLogon(const int idx){
    if (-1 != g_map[idx].fd &&  //connected
            1 == g_map[idx].DevInfo.if_online)  //log on
        return 1;
    else
        return 0;
}

//已连接但还不在线的设备
inline int ResourceIdle(const int idx){
    if (-1 != g_map[idx].fd &&  //connected
            0 == g_map[idx].DevInfo.if_online)  //not login
        return 1;
    else
        return 0;
}

//断开与设备的连接并释放数组资源清空设备信息
void ReleaseResource(const int a){  //idle or log on => available

    syslog(LOG_DEBUG, "removing client on fd %d on idx %d", g_map[a].fd,a);

    close(g_map[a].fd);//关闭与设备的连接
    g_map[a].fd = -1;//清空数组资源

    if(g_map[a].flag != NULL)
        clear_RTEFlag_101(g_map[a].flag);//释放已经申请的内存

    g_map[a].DevInfo.dev_sn   =  0;
    g_map[a].DevInfo.dev_port =  0;
    //g_map[a].DevInfo.dev_addr =  0;
    g_map[a].DevInfo.dev_public_addr = 0;
    g_map[a].DevInfo.if_online = 0;

    //g_map[a].port = -1;
    //g_map[a].ipstr[0] = 0; //'\0'
    PrintResource(a);//更新当前数组信息到调试文件中的对应行

}

//查找可用的数组序列，并填充socket id
inline int FindResource(const int fd){  //avial => idle
    for (int a = 0; a<MAX_DEV_NUM; a++){
        if(ResourceAvail(a)){//当前数组是否未被占用
            g_map[a].fd = fd;

            socklen_t len;
            struct sockaddr_storage addr;

            len = sizeof addr;
            getpeername(fd, (struct sockaddr*)&addr, &len);//获取socket对方地址

            // deal with both IPv4 and IPv6:
            if (addr.ss_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&addr;
                g_map[a].port = ntohs(s->sin_port);
                inet_ntop(AF_INET, &s->sin_addr, g_map[a].ipstr, sizeof g_map[a].ipstr);
            } else { // AF_INET6
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
                g_map[a].port = ntohs(s->sin6_port);
                inet_ntop(AF_INET6, &s->sin6_addr, g_map[a].ipstr, sizeof g_map[a].ipstr);
            }
            PrintResource(a);//更新当前数组信息到调试文件中的对应行
            return a;
        }
    }
    return MAX_DEV_NUM;
}

//填充已经登记设备的设备数据
inline int FindDevInfo(s_com *pCom){  
    unsigned int dev_id=0;
    device_get_id(pCom->flag,&dev_id );//获取设备ID
    for(int i=0;i<MAX_DEV_NUM;i++){
        if(g_pdev[i].DevicePort == dev_id){//已经登记的设备
            pCom->DevInfo.dev_sn   =  g_pdev[i].DeviceSn;
            pCom->DevInfo.dev_port =  atoi(FESPORT);  
            pCom->DevInfo.dev_addr =  g_pdev[i].DevicePort ;
            pCom->DevInfo.dev_public_addr = g_pdev[i].PublicAddr;
            pCom->DevInfo.if_online = 1;
            return 0;
        }
    }
    syslog(LOG_INFO, "dev id(%d) is not in the legal dev id list",dev_id);
    return -1;
}

inline int LoginResource(const int idx, char *buf, int len){  //idle =>log on
/*
    if(init_RTEFlag_101(&(g_map[idx].flag)) != 0){
        syslog(LOG_ERR, "init RTEFlag failed,login failed!");
        return -1;
    }
*/

    if(!ResourceIdle(idx)){//不是已连接但还不在线的设备
        syslog(LOG_WARNING, "Resource(%d) is not idle,cannot login!",idx);
        return -1;
    }
        

    if (login_Proc_101(g_map[idx].flag, buf, &len) == 0 
            && FindDevInfo(&g_map[idx]) == 0 ){////填充已经登记设备的设备数据
        PrintResource(idx);//更新当前数组信息到调试文件中的对应行
        return 0;
    }
    return -1;
}
//打印获取到的所有设备ID
inline int PrintLegalDev(){

    FILE *f = fopen(LISTNAME,"w");
    if(f==NULL){
        syslog(LOG_ERR, "print legal device list failed:%s",strerror(errno));
        return -1;
    }

    for(int i=0;i<MAX_DEV_NUM;i++){
        //sprintf(dev+strlen(dev),"%d ",g_pdev[i].DevicePort);
        if(g_pdev[i].C101_Addrbytes == 0)
            break;
        char buf[32]={};
        sprintf(buf,"%d\n",g_pdev[i].DevicePort);
        int rtn = fwrite(buf,1,strlen(buf),f);
        if(rtn != (int)(strlen(buf))){
            syslog(LOG_ERR,"fwrite failed:%s",strerror(errno));
            int ret = fclose(f);
            if(ret != 0)
                syslog(LOG_ERR, "fclose failed:%s",strerror(errno));
            return -1;
        }
    }

   int ret = fclose(f);
    if(ret != 0)
        syslog(LOG_ERR, "fclose failed:%s",strerror(errno));
    return 0;
}


//更新当前数组信息到调试文件中的对应行
int PrintResource(int idx){
    if(idx >= MAX_DEV_NUM || idx < 0){
        syslog(LOG_WARNING, "invalid idx %d in PrintResource",idx);
        return -1;
    }
        
    //got time tag
    time_t current_time;
    struct tm * time_info;
    char timeString[64];  // space for "HH:MM:SS\0"


    int RETVAL = 0;

    time(&current_time);
    time_info = localtime(&current_time);

    strftime(timeString, sizeof(timeString), "%a, %d %b %y %T", time_info);


    FILE *f = fopen(RESOURCENAME,"r+");
    if(f==NULL) {//deal with the case RESOURCENAME does not exist
        if (ENOENT == errno){
            f = fopen(RESOURCENAME,"w");
            if(f == NULL){
                syslog(LOG_ERR, "open resource file failed:%s",strerror(errno));
                return -1;
            }
        }
        else{
            syslog(LOG_ERR, "open resource file failed:%s",strerror(errno));
            return -1;
        }
    }
   
    

#define TIMELEN "23"//显示时间占用的长度
#define TAGLEN "5"//显示设备状态占用的长度
#define IDXLEN "4"//显示数组序号占用的长度
#define DEVIDLEN "4"//显示数组设备ID占用的长度
#define ONLINELEN "2"//显示在线状态占用的长度
#define IPLEN "15"//显示IP地址占用的长度
#define FDLEN "4"//显示socket fd占用的长度
#define PORTLEN "5"//显示端口占用的长度
#define DASHLEN 1//显示数组序号占用的长度
#define ENTERLEN 1//显示数组序号占用的长度
#define SEPERATORLEN 6//显示数组序号占用的长度

    long linelen = atoi(TIMELEN) + atoi(TAGLEN) + atoi(IDXLEN) + 
        atoi(DEVIDLEN) +  atoi(ONLINELEN) + atoi(IPLEN) + atoi(FDLEN) +
        atoi(PORTLEN)+DASHLEN +ENTERLEN+SEPERATORLEN;


    for(int i=0;i<=idx;i++){
        //if(ResourceAvail(i) == 0)
        //    continue;

        if(i != idx){
            fseek(f,linelen,SEEK_CUR);
            continue;
        }
        else{


            char buf[128]={};
            char tag[16]={};
            if(ResourceAvail(i))//当前数组是否未被占用
                sprintf(tag,"Avail");
            if(ResourceIdle(i))//已连接但还不在线的设备
                sprintf(tag," Idle");
            if(ResourceLogon(i))//设备是否已经登录
                sprintf(tag,"Logon");

            sprintf(buf,"%"TIMELEN"s,%"TAGLEN"s,%"IDXLEN"d,%"DEVIDLEN"d,%"ONLINELEN"d,%"FDLEN"d,%"IPLEN"s-%"PORTLEN"d\n",timeString,tag,i,g_map[i].DevInfo.dev_addr,g_map[i].DevInfo.if_online,g_map[i].fd,g_map[i].ipstr,g_map[i].port); 
            printf("%s",buf);
            int rtn = fwrite(buf,1,strlen(buf),f);
            if(rtn != (int)(strlen(buf))){
                syslog(LOG_ERR,"fwrite failed:%s",strerror(errno));
                RETVAL = -1;
                goto RET;
            }
            else{
                RETVAL = 0;
                goto RET;
            }
        }
    }
    int ret = -1;

RET:
    ret = fclose(f);
    if(ret != 0)
        syslog(LOG_ERR, "fclose failed:%s",strerror(errno));
    return RETVAL;
}

////创建与图形的socket连接
int initHMISock(){

    RemovePrintFiles();//删除/run/fes下的调试文件

    int sock_fd=socket(AF_INET,SOCK_STREAM,0); // AF_INET:Internet;SOCK_STREAM:TCP
    if(sock_fd == -1) // AF_INET:Internet;SOCK_STREAM:TCP
    { 
        syslog(LOG_ERR,"Socket Error:%s",strerror(errno)); 
        exit(1); 
    } 

    char ipAddress[16]="0.0.0.0";

    struct sockaddr_in server_addr; 
    bzero(&server_addr,sizeof(server_addr)); 
    server_addr.sin_family=AF_INET; 
    server_addr.sin_addr.s_addr=inet_addr((char *)ipAddress); 
    server_addr.sin_port=htons(WWWPORT);//与图形通信的端口
    int ret=-1;
    do{
        ret = connect(sock_fd,(struct sockaddr *)(&server_addr),sizeof(struct sockaddr));
        if(ret!=0){
            syslog(LOG_INFO, "connect:%s",strerror(errno));
            sleep(5);
        }
        else{
            syslog(LOG_INFO, "connect www server %s:%d successfully!",ipAddress,WWWPORT);
        }
    }while(ret!=0);

    return sock_fd;
}
//初始化私有协议
void  initIProtocol(){
    init_SCADA_prot(FESIP, atoi(FESPORT));//传递前置的IP地址和与设备通信的端口号
    int rtn=-1;
    char buf[MAXBUFFERSIZE]={};

    do{
        Iprot_logon(buf,1024);//填充登录帧Type=1
        //printf("=>:%s\n",buf);
        SENDOUT(HMI_fd,buf, strlen(buf));//发送登录帧到图形
        rtn = ReadUntil(HMI_fd, '#', 5,buf,MAXBUFFERSIZE);//读取图形发过来的数据

    }while(rtn != 0);
    syslog(LOG_DEBUG,"<=%s",buf); //do nothing, just log what is received 

    memset(buf,0,1024); 

    do{
        //get db dev
        Iprot_get_db_dev(buf,1024);//填充查询帧，获取当前接入终端的所有参数，组帧，请求设备参数，type=2
        //printf("=>:%s\n",buf);
        SENDOUT(HMI_fd,buf, strlen(buf));
        rtn = ReadUntil(HMI_fd, '#', 5,buf,MAXBUFFERSIZE);//读取图形发过来的数据
    }while(rtn != 0);


    //printf("<=%s\n",packet);

    rtn = Protocol_Packet_Recv(buf,strlen(buf)); //解析图形数据，将所有设备信息保存到本地
    //printf("Protocol_Packet_Recv(dev_info) return %d\n",rtn);
    //printRTNtype(rtn);

    int devnum=-1;

    devnum = Iprot_getDevInfo(&g_pdev); //获取设备信息，返回设备总数
    if(devnum <= 0 || g_pdev == NULL){
        syslog(LOG_WARNING,"no dev info is got");
        exit(EXIT_FAILURE);
    }
    else{
        if(devnum > MAX_DEV_NUM){
            syslog(LOG_ERR,"too many devices(%d) from HMI.",devnum);
            exit(EXIT_FAILURE);
        }
    }


    char dev[MAX_DEV_NUM * 16]={}; //no risk of memory crash,see this 2,147,483,647
    sprintf(dev,"dev id list(%d):",devnum);
    for(int i=0;i<devnum;i++){
        sprintf(dev+strlen(dev),"%d ",g_pdev[i].DevicePort);
    }
    sprintf(dev+strlen(dev),"\n");
    syslog(LOG_INFO,"%s",dev);
    PrintLegalDev();//打印获取到的所有设备ID
    /*
       printf("[%d]SN:%lu\n",i,g_pdev[i].DeviceSn);
       printf("[%d]Deviceaddr:%s\n",i,g_pdev[i].DeviceAddr);
       printf("[%d]publicaddr:%d\n",i,g_pdev[i].PublicAddr);
       printf("[%d]C101_Addrbytes:%d\n",i,g_pdev[i].C101_Addrbytes);
       printf("[%d]C101_Cotbytes:%d\n",i,g_pdev[i].C101_Cotbytes);
       printf("[%d]C101_Infobytes:%d\n",i,g_pdev[i].C101_Infobytes);
       printf("[%d]Call_Intervaltime:%d\n",i,g_pdev[i].Call_Intervaltime);
       printf("[%d]C101_Jump_time:%d\n",i,g_pdev[i].C101_Jump_time);
       printf("[%d]Time_Interval:%d\n",i,g_pdev[i].Time_Interval);
       printf("[%d]Time_Out:%d\n",i,g_pdev[i].Time_Out);
       printf("[%d]Repeat_Intervaltime:%d\n",i,g_pdev[i].Repeat_Intervaltime);
       printf("[%d]Repeat_Count:%d\n",i,g_pdev[i].Repeat_Count);
       printf("[%d]Yc_Count:%d\n",i,g_pdev[i].Yc_Count);
       printf("[%d]Yx_Count:%d\n",i,g_pdev[i].Yx_Count);
     */
}

//读取图形发过来的数据
int ReadUntil(const int fd, const char c, const int timeout,char *packet, const int size ){  //timeout in second

    //char packet[MAXBUFFERSIZE];

    memset(packet,0,size);
    int count = 0;


    do{//读取图形发过来的数据
        struct timeval tv;
        fd_set readfds;
        /* Wait up to five seconds. */
        tv.tv_sec = timeout;
        tv.tv_usec = 0;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);//将图形的fd加入监听
        int result = select(fd+1, &readfds, NULL, NULL, &tv);
        if(result  == -1)
            syslog(LOG_ERR, "select error:%s",strerror(errno));
        else if(result == 0){
            //timeout
            return -1; 
        }
        else{
            char buf[1024]={};
            int rtn = read(HMI_fd,buf,1024);//读取图形发过来的数据
            if(rtn == -1){
                syslog(LOG_ERR,"read error:%s",strerror(errno));
                exit(EXIT_FAILURE);
            }
            else if(rtn == 0){//与图形的连接断开
                syslog(LOG_ERR,"the connection is broken");
                exit(EXIT_FAILURE);
            }
            else{
                if((count + rtn) > size)     
                    return -1;
                sprintf(packet+count,"%s",buf);
                count+=rtn;
            }
        }
    }
    while(packet[count-1] != c);//读取图形发过来的数据
    return 0;
}
//向fd写数据
void SENDOUT(int sock_fd,char *buf, int len){
    int sent=0;
    while (sent != len){
        int ret = write(sock_fd, buf+sent, len-sent);
        if (ret == -1){
            syslog(LOG_ERR,"error happens in writing:%s",strerror(errno));
            return;
        }
        sent +=ret;
    }
}

//删除/run/fes下的调试文件
void RemovePrintFiles(){
    int rtn = remove(RESOURCENAME);
    if(rtn != 0)
        syslog(LOG_ERR, "remove %s failed:%s",RESOURCENAME,strerror(errno));

    rtn = remove(LISTNAME);
    if(rtn != 0)
        syslog(LOG_ERR, "remove %s failed:%s",LISTNAME,strerror(errno));

/*

    rtn = remove(NOVERSION);
    if(rtn != 0)
        syslog(LOG_ERR, "remove %s failed:%s",NOVERSION,strerror(errno));

    rtn = remove(TAGNAME);
    if(rtn != 0)
        syslog(LOG_ERR, "remove %s failed:%s",TAGNAME,strerror(errno));

*/

}

void TagVersion(){


    FILE *f=NULL;

    if(strlen(TAGNAME) == strlen("/run/fes/")){
        f = fopen(NOVERSION,"w");
        syslog(LOG_INFO, "TAG:%s",NOVERSION);
    }
    else{
        f = fopen(TAGNAME,"w");
        syslog(LOG_INFO, "TAG:%s",TAGNAME);
    }
        

    if(f==NULL){
        syslog(LOG_ERR, "open tag file failed:%s",strerror(errno));
        return ;
    }
    fclose(f);
}


void InitDevFlag(){
    for(int i=0;i<MAX_DEV_NUM;i++){
        if(init_RTEFlag_101(&(g_map[i].flag)) != 0){
            syslog(LOG_ERR, "init 101 flag failed,i=%d,EXIT",i);
            exit(EXIT_FAILURE);
        }
    }
}

