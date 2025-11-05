#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<arpa/inet.h>
#include<pthread.h>
#define MAX 80
#define PORT 8900
struct sockaddr_in client_addr;
socklen_t len=sizeof(client_addr);
int sockfd;
void *recvmsg(void * args){
	char buff[MAX];
	while(1){
	bzero(buff,MAX);
	int n=recvfrom(sockfd,buff,sizeof(buff),0,(struct sockaddr *)&client_addr,&len);
	if(n>0){
		buff[n]='\0';
		printf("From client: %s",buff);
		fflush(stdout);
		if(strncmp(buff,"exit",4)==0){
			printf("Exit");
			break;
		}
	}
	}
	return NULL;
}
void *sendmsg(void *args){
	char buff[MAX];
	while(1){
		bzero(buff,MAX);
		int n=0;
		while((buff[n++]=getchar())!='\n');
		sendto(sockfd,buff,sizeof(buff),0,(struct sockaddr *)&client_addr,len);
		if (strncmp("exit", buff, 4) == 0)
        {
            printf("Server exit....\n");
            break;
        }
    }
    return NULL;
}
int main(){
	struct sockaddr_in serv_addr;
	pthread_t send_thread,recv_thread;
	
	serv_addr.sin_family=AF_INET;
	serv_addr.sin_addr.s_addr=inet_addr("127.0.0.1");
	serv_addr.sin_port=htons(PORT);
	if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
        {
        	perror("Bind failed");
        	exit(1);
        }
        printf("Server listening on UDP port %d...\n", PORT);
        
        char hello[MAX];
        int n = recvfrom(sockfd, hello, sizeof(hello), 0, (struct sockaddr *)&cliaddr, &len);
    	hello[n] = '\0';
    	
    	printf("Client connected: %s\n", inet_ntoa(cliaddr.sin_addr));
    	
    	pthread_create(&send_thread,NULL,sendmsg,NULL);
    	pthread_create(&recv_thread,NULL,recvmsg,NULL);
    	
    	pthread_join(send_thread,NULL);
    	pthread_join(send_thread,NULL);
    	
	close(sockfd);
	return 0;
}
