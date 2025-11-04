#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<sys/stat.h>
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 7610
#define MAX_DATA 1024
struct Packet{
int seq_num;
int size;
char filename[200];
char data[MAX_DATA];
};
struct Ack{
int seq_num;
char filename[200];
};

int main(){
	int sock;
	struct sockaddr_in server_addr,client_addr;
	socklen_t addr_len=sizeof(client_addr);
	struct Packet packet;
	struct Ack ack;
	FILE *fp=NULL;
	int expected_seq=0;
	char current_file[100]={0};
	
	if((sock=socket(AF_INET,SOCK_DGRAM,0))==-1){
		perror("Socket");
		exit(1);
	}
	
	server_addr.sin_family=AF_INET;
	server_addr.sin_addr.s_addr=inet_addr(SERVER_IP);
	server_addr.sin_port=htons(SERVER_PORT);
	
	if(bind(sock,(struct sockaddr *)&server_addr,sizeof(struct sockaddr))==-1){
		perror("Bind");
		exit(1);
	}
	
	printf("UDP Folder Server (Resume supported) listening on %s: %d..\n",SERVER_IP,SERVER_PORT);
	mkdir("received_folder",0777);
	
	while(1){
		int bytes=recvfrom(sock,&packet,sizeof(packet),0,(struct sockaddr *)&client_addr,&addr_len);
		
		if(bytes<=0) continue;
		
		if(packet.seq_num==-999){
		printf("\n[+] Folder transfer complete\n");
		if(fp) fclose(fp);
		break;
		}
		
		if(packet.seq_num==-1){
		printf("\n[+] File %s transfer complete\n",current_file);
		if(fp) fclose(fp);
		fp=NULL;
		expected_seq=0;
		continue;
		}
		
		if(fp==NULL || strcmp(current_file,packet.filename)!=0){
		char new_filepath[200];
		snprintf(new_filepath,sizeof(new_filepath),"received_folder/%s",packet.filename);
		
		fp=fopen(new_filepath,"ab");
		if(!fp){
		perror("File open");
		exit(1);
		}
		
		strcpy(current_file,packet.filename);
		expected_seq=0;
		printf("[+] Receiving/resuming file: %s\n",new_filepath);
		}
		
		if(packet.seq_num==expected_seq){
		fwrite(packet.data,1,packet.size,fp);
		printf("Received %d [%d bytes] of %s",packet.seq_num,packet.size,packet.filename);
		ack.seq_num=expected_seq;
		strcpy(ack.filename,packet.filename);
		sendto(sock,&ack,sizeof(ack),0,(struct sockaddr *)&client_addr,addr_len);
		expected_seq++;
		}
		else{
		ack.seq_num=expected_seq-1;
		strcpy(ack.filename,packet.filename);
		sendto(sock,&ack,sizeof(ack),0,(struct sockaddr *)&client_addr,addr_len);
		}
	}
	close(sock);
	return 0;
}		
		
		
		
		
		
