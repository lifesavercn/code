#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<netdb.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<pthread.h>
#include<unistd.h>
#define MAX 80
#define PORT 8090
int sockfd;
struct sockaddr_in serv_addr;
socklen_t len=sizeof(serv_addr);
void *recvmsg(void *args){
	char buff[MAX];
	
	while(1){
		bzero(buff,MAX);
		int n=recvfrom(sockfd,buff,sizeof(buff),0,NULL,NULL);
		if(n>0){
			buff[n]='\0';
			printf("From Server: %s", buff);
            	fflush(stdout);
            	if (strncmp("exit", buff, 4) == 0)
            	{
                	printf("Server disconnected...\n");
                	break;
            	}
		}
		
	}
	return NULL;
}
void *sendmsg(void * args){
	char buff[MAX];
	while(1){
	bzero(buff,MAX);
	int n = 0;
	while ((buff[n++] = getchar()) != '\n');
	sendto(sockfd,buff,sizof(buff),0,(struct sockaddr *)&serv_addr,len);
	if (strncmp("exit", buff, 4) == 0)
        {
            printf("Client exit....\n");
            break;
        }
    }
    return NULL;
}

int main(){
	pthread_t send_thread,recv_thread;
	sockfd=socket(AF_INET,SOCK_DGRAM,0);
	socklen_t len=sizeof(serv_addr);
	
	if(sockfd<0){
	perror("Socket error");
	exit(0);
	}
	
	printf("\nUDP Socket created successfully");
	bzero(&serv_addr,sizeof(serv_addr));
	serv_addr.sin_family=AF_INET;
	serv_addr.sin_addr.s_addr=inet_addr("127.0.0.1");
	serv_addr.sin_port=htons(PORT);
	
	char hello[]="Hello from client\n";
	sendto(sockfd,hello,strlen(hello),0,(struct sockaddr *)&serv_addr,len);
	printf("\nConnected to UDP server");
	
	pthread_create(&send_thread,NULL,sendmsg,NULL);
	pthread_create(&recv_thread,NULL,recvmsg,NULL);
	pthread_join(send_thread,NULL);
	pthread_join(recv_thread,NULL);
	close(sockfd);
	return 0;
}
