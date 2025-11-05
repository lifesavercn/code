#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<pthread.h>
void *other_thread(void * args){
	int i;
	for(i=0;i<=10;i++){
		printf("other thread: %d",i);
		sleep(1);
	}
}
void *number_thread(void * args){
	int i;
	for(i=0;i<=9;i++){
		printf("\n%d",i);
		if(i==5){
		pthread_t other;
		pthread_create(&other,NULL,other_thread,NULL);
		pthread_join(other,NULL);
		}
		usleep(200);
	}
	return NULL;
}
void *letter_thread(void * args){
	char c;
	for(c='A';c<='Z';c++){
		printf("\n%c",c);
		usleep(150);
	}
}
int main(){
	pthread_t number,letter;
	printf("\nBefore Thread");
	pthread_create(&number,NULL,number_thread,NULL);
	pthread_create(&letter,NULL,letter_thread,NULL);
	pthread_join(number,NULL);
	printf("\nEnd of number thread");
	pthread_join(letter,NULL);
	printf("\nEnd of letter thread");
	printf("\nAfter Thread");
	
	exit(0);
}
