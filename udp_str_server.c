#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<ctype.h>
#define PORT 5311
#define SIZE 50
void process(char *str){
	int i,n=strlen(str);
	char temp;
	
	for(i=0;i<n/2;i++){
	temp=str[i];
	str[i]=str[n-i-1];
	str[n-i-1]=temp;
	}
	
	for(i=0;i<n;i++){
	if (islower(str[i]))
            str[i] = toupper(str[i]);
        else if (isupper(str[i]))
            str[i] = tolower(str[i]);
	}
}
int main(){
	int sockfd;
	struct sockaddr_in server_addr,client_addr;
	char buff[SIZE];
	socklen_t len=sizeof(client_addr);
	
	server_addr.sin_family=AF_INET;
	server_addr.sin_addr.s_addr=inet_addr("127.0.0.1");
	server_addr.sin_port=htons(PORT);
	sockfd=socket(AF_INET,SOCK_DGRAM,0);
	if(bind(sockfd,(struct sockaddr *)&server_addr,sizeof(server_addr))==-1){
	perror("Bind");
	exit(0);
	}
	
	printf("\nUDP SERVER LISTENING");
	int n=recvfrom(sockfd,(char *)buff,sizeof(buff),0,(struct sockaddr *)&client_addr,&len);
	buff[n]='\0';
	
	printf("[\nServer] String received: %s",buff);
	process(buff);
	
	printf("\n[Server]Sending back to client");
	sendto(sockfd,buff,sizeof(buff),0,(struct sockaddr *)&client_addr,len);
	
	close(sockfd);
	return 0;
}		
