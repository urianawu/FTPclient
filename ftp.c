#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <net/if.h>
#include <time.h>
#include <sys/stat.h>


// Maximum number of clients
#define MAX_CLIENT 128

//Root account shall be used for the following 2 ports 
#define SERVER_CMD_PORT 21
#define SERVER_DAT_PORT 20

//buff size for data transfer in data connection
#define BIG_SIZE 1024

//Mode to set up data connections, Active or Passive
enum data_conn_mode {
	kDataConnModeActive, kDataConnModePassive
}; //enumerate

//Mode to set up data transfer type, Binary or Ascii
enum data_transfer_type {
	ascii, binary
};

//Structure to store client information: name, IP address, ...
struct Client {
	int accountNum;
	char user[128];
	char pass[128];
	int sockfd; //For data connection
	unsigned short data_port;
	struct in_addr sin_addr; //32-bit IP address for IPv4
	enum data_conn_mode mode;
	enum data_transfer_type type;
	char cwd[128]; //Client Work Directory
	char hd[128]; // Client home directory
	char rnfrName[124]; //old name for rename commend
	char rntoName[124]; //new name for rename commend
	int traffic;//accumulated traffic
	int auth;//authority
	int pasvfd;
	//Other information

};

//User account
int acc;
struct Account {
	char user[128];
	char pass[128];
	int auth; //authority of this account,0 for root,1 for download only,2 for upload only.
};

struct Account account[MAX_CLIENT];

struct sockaddr_in servAddr; /* Local address */
struct sockaddr_in datAddr; /* data transfer address in passive mode*/

//used to calculate time
/*struct timeval {
	long tv_sec;//seconds
	long tv_usec;//microseconds
};*/

#define speedMaxDown 500//Max spped when downloading
#define speedMaxUp 300//Max speed when uploading

char home_dir[128]; //Server Home Directory

//Passive mode port number = pasvP1 * 256 + pasvP2
int pasvP1, pasvP2;

//Used to listen for command connection;
int server_sockfd;
//Used to store the socket-fd returned by accept();
int client_sockfd;

/*Array of client structure, client_socketfd is used as index.
For example, if client_socketfd of a client is 4, then the structure of client[4] stores his information. */
struct Client client[MAX_CLIENT];

//Creat a socket for server, server_sockfd is given a value
int create_server_socket();

// Set up data connection in Active mode
int data_conn_active(struct in_addr *sin_addr, unsigned short port);
	
//Set up data connection in Passive mode
int data_conn_passive(unsigned short port);

// Reaction of command LIST or NLST: Sending directory and file list;
// dir is an absolute path
int send_list(int sockfd, char *dir);


int main()
{

	acc = 0;

	memset(&account[0], 0, sizeof(struct Account));
	strcpy(account[0].user, "root");
	strcpy(account[0].pass, "root");
	account[0].auth = 0; //full authority
	acc++;

	memset(&account[1], 0, sizeof(struct Account));
	strcpy(account[1].user, "guest");
	strcpy(account[1].pass, "guest");
	account[1].auth = 1; //download only
	acc++;
	
	memset(&account[2], 0, sizeof(struct Account));
	strcpy(account[2].user, "student");
	strcpy(account[2].pass, "student");
	account[2].auth = 2; //upload only
	acc++;

	int client_len; 
	struct sockaddr_in client_address;
	int result;
	fd_set readfds, testfds; // a set of sockets to be monitored

	//create a socket and server_sockfd will be set.
	create_server_socket( );

	//create a virtual home directory if no existing
	memset(home_dir, 0, sizeof(home_dir));
	getcwd(home_dir, sizeof(home_dir));
	home_dir[strlen(home_dir)] = '/';
	strcat(home_dir, "ftp_home");
	mkdir("ftp_home", 0777);
	if (chdir(home_dir) != 0)
		printf("virtual home directory initialization failure\n");

	//generate a port number for passive mode
	//every time data is transfered in passive mode, this number increases by one
	pasvP1 = rand()%200 + 10;
	pasvP2 = rand()%255 + 0;

	FD_ZERO(&readfds);  //clear fd_set 
	FD_SET(server_sockfd, &readfds); // add server_sockfd to fd_set

	while(1) {
		char buf[128];
		int nread;
		int fd;

		testfds = readfds; //used to store readfds temporarily
		printf("server waiting\n");

		/* select(int maxfd, fd_set *readset, fd_set *writeset, fd_set *exceptset, const struct timeval *timeout) is used to monitor events of 
		    a set of sockets. If timeout sets to NULL, wait forever and return only when one of the descriptors is ready for I/O. 
		    A return value <0 indicates an error, 0 indicates a timeout. */
		result = select(FD_SETSIZE, &testfds, (fd_set *)0, (fd_set *)0, (struct timeval *)0); 
		if(result< 0) {
			perror("ftp_server"); //output the reason for function failure to standard equipment
			exit(1);
		}

		for(fd = 0; fd < FD_SETSIZE; fd++) {
			// checking which socket triggers a read event.
			if(FD_ISSET(fd, &testfds)) {//If fd is subset of fd_set, return true
				if(fd == server_sockfd) {
					// Processing new connection requests.
					client_len = sizeof(client_address);
					client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_address,&client_len);
					// add server_sockfd to fd_set
					FD_SET(client_sockfd, &readfds); 
					client[client_sockfd].sin_addr.s_addr = client_address.sin_addr.s_addr;
					printf("adding client on fd %d\n", client_sockfd);
					sprintf(buf, "220 (wxftp 1.0)\r\n"); //write formatted data into the string, data can be any type
					write(client_sockfd, buf, strlen(buf)); //write n bytes data in buf into file descriptor
				} 
				else {
					//Processing commands from client
					// check if anything received from socket fd;
					// nread  stores number of bytes received.
					ioctl(fd, FIONREAD, &nread); //get the number of bytes in cash 	
					client[fd].sockfd = -1;
					if(nread == 0) {//quit command
						/* no data received, indicating the client has closed FTP command connection. */
						close(fd);
						memset(&client[fd], 0, sizeof(struct Client));
						FD_CLR(fd, &readfds); //clear fd
						printf("removing client on fd %d\n", fd);
					} 
					else {
						//read commands from socket fd and process;
						read(fd, buf, nread);
						buf[nread] = '\0';
						printf("serving client on fd %d: %s\n", fd, buf);

						if(strncmp(buf, "USER", 4) == 0) {
							// Receiving user name;
							// You need to check if it is a valid user; 
							sscanf(&buf[4], "%s", client[fd].user); //read data of specifid type from string
							int i;
							for (i = 0; i < acc ; i++) {
								if (strncmp(client[fd].user,account[i].user,sizeof(account[i].user))==0){ //check if valid
									client[fd].accountNum = i;
									sprintf(buf, "331 Password required for %s.\r\n", client[fd].user);
									write(fd, buf, strlen(buf));
								}
							} 
						}
						else if(strncmp(buf, "PASS", 4) == 0) {
							// Authenticate the user
							sscanf(&buf[4], "%s", client[fd].pass);
							if (strncmp(client[fd].pass, account[client[fd].accountNum].pass, sizeof(account[client[fd].accountNum].pass)) == 0) { //check Password
								sprintf(buf, "230 User %s logged in.\r\n", client[fd].user);
								write(fd, buf, strlen(buf));
								client[fd].auth = account[client[fd].accountNum].auth;
								//create a virtual home directory if no existing
								strcpy(client[fd].hd, home_dir); // home directory
								client[fd].hd[strlen(client[fd].hd)] = '/';
								strcat(client[fd].hd, client[fd].user);
								mkdir(client[fd].hd, 0777);
								if (chdir(client[fd].hd) != 0)
									printf("user home directory initialization failure\n");
								getcwd(client[fd].cwd, sizeof(client[fd].cwd));// current directory
							}
						} 
						else if(strncmp(buf, "PWD", 3) == 0 || strncmp(buf, "XPWD", 4) == 0) {
							//Prosessing PWD command, u: print working directory
							chdir(client[fd].cwd);
							sprintf(buf, "257 %s is current directory\r\n", client[fd].cwd);
							write(fd, buf, strlen(buf));
						} 
						else if(strncmp(buf, "SYST", 4) == 0) {							
							//Prosessing SYST command
							sprintf(buf, "215 Unix\r\n");
							write(fd, buf, strlen(buf));
						} 
						else if(strncmp(buf, "CWD", 3) == 0) {
							//Procssing CWD command
							chdir(client[fd].cwd);
							char dir[124];
							char tmpd[124];
							memset(dir, 0, sizeof(dir));
							sscanf(&buf[3], "%s", dir);
							if (!dir) 
								sprintf(buf, "550 Error encountered.\r\n");
							if (dir[0] == '.' && dir[1] == '.') {
								int i;
								memset(tmpd, 0, sizeof(tmpd));
								strcpy(tmpd,client[fd].cwd);
								for (i = strlen(tmpd)-1 ; i >= 0 ; i--) {
									if (tmpd[i] = '/') {
										tmpd[i] = '\0';
										break;
									}
								}
								
								if (strcmp(tmpd,client[fd].hd) >= 0){
									if (chdir(tmpd) == 0){
										sprintf(buf, "250 CWD successful. \"%s\" is current directory\r\n", tmpd);
									}
								else {
									chdir(client[fd].hd);
									sprintf(buf, "250 CWD successful. \"%s\" is current directory\r\n", client[fd].hd);
								}
								}
								
							}
							/* Absolute path */
							if (dir[0] == '/') {
								memset(tmpd, 0, sizeof(tmpd));
								strcpy(tmpd,client[fd].hd);
								strcat(tmpd, dir);// user change path in its own directory
								
								
								if (chdir(tmpd) == 0) {
									sprintf(buf, "250 CWD successful.\r\n");
								}
								else {
									sprintf(buf, "550 No such file or invalid path.\r\n");
								}
							}
							/* Relative path */
							if (dir[0] != '/' && dir[1] != '.') {
								if (chdir(dir) == 0) {
									sprintf(buf, "250 CWD successful.\r\n");
								}
								else
									sprintf(buf, "550 No such file.\r\n");
							}
							getcwd(client[fd].cwd, sizeof(client[fd].cwd));
							write(fd, buf, strlen(buf));
						} 
						else if(strncmp(buf, "CDUP", 4) == 0) {	
							//Processing CDUP command
							chdir(client[fd].cwd);
							if (strcmp(client[fd].cwd, client[fd].hd) == 0)
								sprintf(buf, "200 CDUP successful.\r\n");
							else {
								char dir[128];
								memset(dir, 0, sizeof(dir));
								strcpy(dir, client[fd].cwd);
								int i = strlen(dir) - 1;
								while (dir[i] != '/')
									i--;
								dir[i] = '\0';
								if (chdir(dir) == 0) {
									getcwd(client[fd].cwd, sizeof(client[fd].cwd));
									sprintf(buf, "200 CDUP successful.\r\n");
								}
								else
									 sprintf(buf, "550 Error encountered.\r\n");
							}
							write(fd, buf, strlen(buf));	
						} 
						
						
						
						else if(strncmp(buf, "PASV", 4) == 0) {
							//Processing PASV commmand
							
							
							char servIP[20];
							if (getlocalip(servIP) != 0)
								printf("Unable to read IP address.\n");

							//write ip into tmp
							char tmp[25];
							memset(tmp,0,sizeof(tmp));
							int i;
							for (i = 0 ; servIP[i] != 0 ; i++) {
								if (servIP[i] != '.')
									tmp[i] = servIP[i];
								else
									tmp[i] = ',';
							}
							tmp[i] = ',';

							//generate a new passive mode port number
							if (pasvP2 < 255)
								pasvP2++;
							else {
								pasvP2 = 0;
								pasvP1++;
							}
							client[fd].data_port = pasvP1*256 + pasvP2;

							//convert an integer to string and write into tmp
							char string[4];
							memset(string, 0, sizeof(string));
							sprintf(string, "%d", pasvP1);
							strcat(tmp, string);
							tmp[strlen(tmp)] = ',';
							memset(string, 0, sizeof(string));
							sprintf(string, "%d", pasvP2);
							strcat(tmp, string);
							

							client[fd].mode = kDataConnModePassive;
							client[fd].pasvfd = data_conn_passive(client[fd].data_port);
							sprintf(buf, "227 Entering Passive Mode (%s)\r\n", tmp);
							write(fd, buf, strlen(buf));
							
						} 
						
						else if(strncmp(buf, "PORT", 4) == 0) {
							//Processing PORT command;
							/* store IP address of the client in client[fd].sin_addr; store port number following PORT command in client[fd].data_port; set client[fd].mode as kDataConnModeActive;... */
							char tmp[25];
							memset(tmp,0,sizeof(tmp));
							char cltIP[16];
							memset(cltIP,0,sizeof(cltIP));
							char cltPortP1[4];
							char cltPortP2[4];
							memset(cltPortP1,0,sizeof(cltPortP1));
							memset(cltPortP2,0,sizeof(cltPortP2));
							unsigned short cltPort;
							sscanf(&buf[4], "%s", tmp);
							int iplen = strlen(tmp);
							int i,p1,p2;
							int IPdotNum=3;
							int portCommaNum = 1;
							for (i=0, p1=0, p2=0 ; iplen > 0 ; iplen--){
								if (IPdotNum > -1) {
									if (tmp[i] != ',')
										cltIP[i] = tmp[i];
									else {
										if (IPdotNum > 0)
											cltIP[i] = '.';
										IPdotNum--;
									}
									i++;
								}
								else {
									if (portCommaNum > 0) {
										if (tmp[i] != ',')
											cltPortP1[p1] = tmp[i];
										else
											portCommaNum--;
										i++;
										p1++;
									}
									else {
										cltPortP2[p2] = tmp[i];
										i++;
										p2++;
									}
								}
							}
							client[fd].sin_addr.s_addr = inet_addr(cltIP);
							cltPort = atoi(cltPortP1)*256 + atoi(cltPortP2);
							printf("IP: %s  Port: %d\n", cltIP, cltPort);
							client[fd].data_port = htons(cltPort);
							client[fd].mode = kDataConnModeActive;
							sprintf(buf, "200 PORT command successful.\r\n");
							write(fd, buf, strlen(buf));
						} 
						

						else if(strncmp(buf, "LIST", 4) == 0 || strncmp(buf, "NLST", 4) == 0) {//TBD
							//Processing LIST or NLST command

							chdir(client[fd].cwd);
							if(client[fd].mode == kDataConnModeActive) {
								client[fd].sockfd = data_conn_active(&client[fd].sin_addr, client[fd].data_port);
							} 
							else if (client[fd].mode == kDataConnModePassive) {

								int datAddrlen = sizeof(datAddr);
								if ((client[fd].sockfd = accept(client[fd].pasvfd, (struct sockaddr *)&datAddr,&datAddrlen)) < 0)
									printf("accept() failed\n");
							}
							int result = 0;
							if (client[fd].sockfd != -1) {
								sprintf(buf,"150 Opening data connection for directory list.\r\n");
								write(fd, buf, strlen(buf));
								if(send_list(client[fd].sockfd, client[fd].cwd) == 0) {
									sprintf(buf, "226 Transfer ok.\r\n");
								} 
								else {
									sprintf(buf, "550 Error encountered.\r\n");
								}
								write(fd, buf, strlen(buf));
								close(client[fd].sockfd);
								client[fd].sockfd = -1;
							}
						} 
					
						else if (strncmp(buf, "RETR", 4) == 0) {
							//Processing RETR command
							if(client[fd].auth!=2){
								chdir(client[fd].cwd);
								char fileName[124];
								sscanf(&buf[4], "%s", fileName);
								if(client[fd].mode == kDataConnModeActive)
									client[fd].sockfd = data_conn_active(&client[fd].sin_addr, client[fd].data_port);
								else if (client[fd].mode == kDataConnModePassive) {
									int datAddrlen = sizeof(datAddr);
									if ((client[fd].sockfd = accept(client[fd].pasvfd, (struct sockaddr *)&datAddr,&datAddrlen)) < 0)
										printf("accept() failed\n");
								}

								if (client[fd].type = binary) {
									int source;
									char buff[BIG_SIZE];
									register int fr;
									if (client[fd].sockfd != -1) {
										sprintf(buf,"150 Opening data connection for %s.\r\n", fileName);
										write(fd, buf, strlen(buf));
										//Begin to transfer
										struct timeval ts, te, tsflag, teflag; //calculate time
										int speed;
										if ((source = open(fileName, O_RDONLY, 0)) < 0) {
											sprintf(buf, "550 open failed\r\n");
											write(fd, buf, strlen(buf));
										}
										lseek(source, 0L, 0);
										memset(buff, 0, sizeof(buff));
										gettimeofday(&ts, 0);//clock starts
										gettimeofday(&tsflag, 0);//flag clock starts
										int data1K = 0;
										int dataflag = 0;
										int i=0;
										int j=0;
										while((fr = read(source, buff, sizeof(buff))) > 0) {
											write(client[fd].sockfd, buff, fr);
											memset(buff, 0, sizeof(buff));
											data1K++;
											dataflag++;
											i++;
											if (i == speedMaxDown) {
												i = 0;
												j = 1;
											}
											while(j) {
												gettimeofday(&teflag, 0);//flag clock ends
												float timeflag = 1000000*(teflag.tv_sec - tsflag.tv_sec) + teflag.tv_usec - tsflag.tv_usec;
												timeflag /= 1000000;
												if (timeflag*speedMaxDown > dataflag) {
													j = 0;
													dataflag = 0;
													gettimeofday(&tsflag, 0);//flag clock starts
												}
												else
													sleep(0.05);
											}
										}
										gettimeofday(&te, 0);//clock ends
										client[fd].traffic += data1K;
										float time = 1000000*(te.tv_sec - ts.tv_sec) + te.tv_usec - ts.tv_usec;
										time /= 1000000;
										speed = data1K/time;
										printf("%d Kbytes sent in %.1f secs (%d KB/s)\n", data1K, time, speed);
										printf("Total traffic is %dK bytes\n", client[fd].traffic);
										close(client[fd].sockfd);
										client[fd].sockfd = -1;
										close(source);
										close(fr);
										sprintf(buf, "226 File sent ok.\r\n");
										write(fd, buf, strlen(buf));
									}
									else {
										sprintf(buf, "550 Error encountered.\r\n");
										write(fd, buf, strlen(buf));
									}
								}
								else {
									char buff[BIG_SIZE];
									FILE *fr;
									int bytes;
									if (client[fd].sockfd != -1) {
										sprintf(buf,"150 Opening data connection for %s.\r\n", fileName);
										write(fd, buf, strlen(buf));
										//Begin to transfer
										struct timeval ts, te; //calculate time
										int speed;
										if ((fr = fopen(fileName, "r")) == NULL) {
											sprintf(buf, "550 open failed\r\n");
											write(fd, buf, strlen(buf));
										}
										int dataInByte = 0;								
										gettimeofday(&ts, 0);//clock starts
										while (fgets(buff, BIG_SIZE, fr) != NULL) {
											bytes = strlen(buff);
											buff[bytes - 1] = '\n';
											write(client[fd].sockfd, buff, bytes);
											dataInByte += bytes;
										}
										gettimeofday(&te, 0);//clock ends
										client[fd].traffic += dataInByte/1024;
										float time = 1000000*(te.tv_sec - ts.tv_sec) + te.tv_usec - ts.tv_usec;
										time /= 1000000;
										speed = dataInByte/time/1024;
										printf("%d Kbytes sent in %.1f secs (%d KB/s)\n", dataInByte/1024, time, speed);
										printf("Total traffic is %d Kbytes\n", client[fd].traffic);
										close(client[fd].sockfd);
										client[fd].sockfd = -1;
										sprintf(buf, "226 File sent ok.\r\n");
										write(fd, buf, strlen(buf));
									}
								}
							}
							else{
							sprintf(buf, "550 This client is not allowed to download.\r\n");
							write(fd,buf,strlen(buf));
							}
						}
						else if (strncmp(buf, "STOR", 4) == 0) {
							//Processing STOR command
							if(client[fd].auth!=1){
							chdir(client[fd].cwd);
							char fileName[124];
							sscanf(&buf[4], "%s", fileName);
							if(client[fd].mode == kDataConnModeActive)
								client[fd].sockfd = data_conn_active(&client[fd].sin_addr, client[fd].data_port);
							else if (client[fd].mode == kDataConnModePassive) {

								int datAddrlen = sizeof(datAddr);
								if ((client[fd].sockfd = accept(client[fd].pasvfd, (struct sockaddr *)&datAddr,&datAddrlen)) < 0)
									printf("accept() failed\n");
							}
							int target;
							char buff[BIG_SIZE];
							register int fw;
							if (client[fd].sockfd != -1) {
								sprintf(buf,"150 Opening data connection for %s.\r\n", fileName);
								write(fd, buf, strlen(buf));
								//Begin to transfer
								struct timeval ts, te, tsflag, teflag; //calculate time
								int speed;
								int data1K = 0;
								int dataflag = 0;
								int i=0;
								int j=0;
								if ((target = open(fileName, O_WRONLY|O_CREAT|O_TRUNC, 0644)) < 0)
									printf("open() failed\n");
								memset(buff, 0, sizeof(buff));
								gettimeofday(&ts, 0);//clock starts
								gettimeofday(&tsflag, 0);//flag clock starts
								while ((fw = read(client[fd].sockfd, buff, sizeof(buff))) > 0) {
									write(target, buff, fw);
									memset(buff, 0, sizeof(buff));
									data1K++;
									dataflag++;
									i++;
									if (i == speedMaxUp) {
										i = 0;
										j = 1;
									}
									while(j) {
										gettimeofday(&teflag, 0);//flag clock ends
										float timeflag = 1000000*(teflag.tv_sec - tsflag.tv_sec) + teflag.tv_usec - tsflag.tv_usec;
										timeflag /= 1000000;
										if (timeflag*speedMaxUp > dataflag) {
											j = 0;
											dataflag = 0;
											gettimeofday(&tsflag, 0);//flag clock starts
										}
										else
											sleep(0.05);
									}
								}
								gettimeofday(&te, 0);//clock ends
								client[fd].traffic += data1K;
								float time = 1000000*(te.tv_sec - ts.tv_sec) + te.tv_usec - ts.tv_usec;
								time /= 1000000;
								speed = data1K/time;
								printf("%d Kbytes received in %.1f secs (%d KB/s)\n", data1K, time, speed);
								printf("Total traffic is %dK bytes\n", client[fd].traffic);
								close(client[fd].sockfd);
								client[fd].sockfd = -1;
								close(target);
								close(fw);
								sprintf(buf, "226 Receive complete.\r\n");
								write(fd, buf, strlen(buf));
							}
							else {
								sprintf(buf, "550 Error encountered.\r\n");
								write(fd, buf, strlen(buf));
								}
							}
							else{
									sprintf(buf, "550 This client is not allowed to upload.\r\n");
									write(fd,buf,strlen(buf));
									}
						} 
	
						else if (strncmp(buf, "QUIT", 4) == 0) {
	
							//Processing QUIT command
							
							sprintf(buf, "221 Goodbye.\r\n");
							write(fd, buf, strlen(buf));
							close(fd);
							memset(&client[fd], 0, sizeof(struct Client));
							FD_CLR(fd, &readfds);
							printf("removing client on fd %d\n", fd);
							chdir(home_dir);//go back to home directory and prepare for new user
						} 
						else if (strncmp(buf, "DELE", 4) == 0) {
							//Processing DELE command
							if(client[fd].auth == 0){
							char fileName[124];
							sscanf(&buf[4], "%s", fileName);
							if ((remove(fileName)) == 0) 
								sprintf(buf, "200 delete %s sucessfully.\r\n", fileName);
							else
								sprintf(buf, "550 File does not exist or permission denied\r\n");
							}
							else
								sprintf(buf, "550 No authority.\r\n");
								
							write(fd, buf, strlen(buf));
							
						}
	
						else if (strncmp(buf, "RNFR", 4) == 0) {
							//Processing RNFR command
							if(client[fd].auth == 0){
							sscanf(&buf[4], "%s", client[fd].rnfrName);
							sprintf(buf, "350 Old name is %s. Wait for new name...\r\n", client[fd].rnfrName);
							}
							else
								sprintf(buf, "550 No authority.\r\n");
							write(fd, buf, strlen(buf));
						} 
						else if (strncmp(buf, "RNTO", 4) == 0) {
							//Processing RNTO command
							if(client[fd].auth == 0){
							sscanf(&buf[4], "%s", client[fd].rntoName);
							if (rename(client[fd].rnfrName, client[fd].rntoName) == 0)
								sprintf(buf, "250 Rename successfully. New name is %s.\r\n", client[fd].rntoName);
							else 
								sprintf(buf, "550 Rename failure\r\n");
							}
							else
								sprintf(buf, "550 No authority.\r\n");
							write(fd, buf, strlen(buf));
						} 
						else if (strncmp(buf, "MKD", 3) == 0 || strncmp(buf, "XMKD", 4) == 0) {
							//Processing MKD command
							if(client[fd].auth == 0){
							char dir[124];
							memset(dir, 0, sizeof(dir));
							char tmp[232];
							memset(tmp, 0, sizeof(tmp));
							sscanf(&buf[4], "%s", dir);
							strcpy(tmp, client[fd].cwd);
							tmp[strlen(tmp)] = '/';
							strcat(tmp, dir);
							if (mkdir(dir, 0777) == 0)
								sprintf(buf, "257 \"%s\" directory created\r\n", tmp);
							else 
								sprintf(buf, "521 \"%s\" directory already exists\r\n", tmp);
							}
							else
								sprintf(buf, "550 No authority.\r\n");
							write(fd, buf, strlen(buf));
						} 
						else if (strncmp(buf, "RMD", 3) == 0 || strncmp(buf, "XRMD", 4) == 0) {
							//Processing RMD command
							if(client[fd].auth == 0){
							char dir[124];
							sscanf(&buf[4], "%s", dir);
							if (rmdir(dir) == 0)
								sprintf(buf, "250 \"%s\" directory removed\r\n", dir);
							else 
								sprintf(buf, "550 rmdir failure\r\n");
							}
							else
								sprintf(buf, "550 No authority.\r\n");
							write(fd, buf, strlen(buf));
						} 
						
						else if (strncmp(buf, "TYPE I", 6) == 0) {
							//Processing TYPE I command
							client[fd].type = binary;
							sprintf(buf, "200 Binary mode.\r\n");
							write(fd, buf, strlen(buf));
						} 

						else if (strncmp(buf, "TYPE A", 6) == 0) {
							//Processing TYPE A command
							client[fd].type = ascii;
							sprintf(buf, "200 Ascii mode.\r\n");
							write(fd, buf, strlen(buf));
						}

						else {
							//Processsing other commands
							sprintf(buf, "502 no such command supporting\r\n");
							write(fd, buf, strlen(buf));
						

						}
					}
				}
			}
		}
	}
	return 0;
}


/* Create a socket for server, which is used to listening on port 21.  Global variable server_sockfd is given a value. */
int create_server_socket()
{
	if ((server_sockfd = socket(AF_INET, SOCK_STREAM, 0))<0)
		printf("socket() failed\n");
	/* Construct local address structure */
	memset(&servAddr, 0, sizeof(servAddr)); 
	servAddr.sin_family = AF_INET;
	servAddr.sin_addr.s_addr = htonl(INADDR_ANY); //Any address,usually all 0s
	servAddr.sin_port =htons(SERVER_CMD_PORT);
	/* Bind to the local address */
	if((bind(server_sockfd, (struct sockaddr *) &servAddr, sizeof(servAddr))) < 0)
		printf("bind() failed.\n");
	/* Listen to accept client connection request */
	if ((listen(server_sockfd, MAX_CLIENT))<0)
		printf("listen() failed.\n");
	return server_sockfd;
}


/* set up data connection in active mode.
   The input parameters are IP address and port number of the client.
   The return value is socket file descriptor created for data connection. */
int data_conn_active(struct in_addr *sin_addr, unsigned short port)
{
	int sockfd;
	struct sockaddr_in cltAddr; /* Client address to be connected */
	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0))<0)
		printf("socket() failed\n");
	/* Construct local data transfer address structure */
	memset(&cltAddr, 0, sizeof(cltAddr)); 
	cltAddr.sin_family = AF_INET;
	cltAddr.sin_addr = *sin_addr;
	cltAddr.sin_port = port;
	if (connect(sockfd, (struct sockaddr *)&cltAddr, sizeof(cltAddr)) < 0)
		printf("connect() failed.\n");
	return sockfd;

	/* Create a socket and connect to client whose IP address and port number is given as input parameters. */ 
printf("data_conn_active() completed\n");
}

/* set up data connection in passive mode.
   The input parameters are port number of the client.
   The return value is socket file descriptor created for data connection. */
int data_conn_passive(unsigned short port) 
{

	char servIP[20];
	if (getlocalip(servIP) != 0)
		printf("Unable to read IP address.\n");

	int sockfd;
	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0))<0)
		printf("socket() failed\n");
	/* Construct local address structure */
	memset(&datAddr, 0, sizeof(datAddr)); 
	datAddr.sin_family = AF_INET;
	datAddr.sin_addr.s_addr = inet_addr(servIP); //Any address,usually all 0s inet_addr
	datAddr.sin_port =htons(port);
	/* Bind to the local address */
	if((bind(sockfd, (struct sockaddr *) &datAddr, sizeof(datAddr))) < 0)//TBD
		printf("bind() failed.\n");
	/* Listen to accept client connection request */
	if ((listen(sockfd, MAX_CLIENT))<0)
		printf("listen() failed.\n");

	return sockfd;

}


	/* Find out directory and file list and send to client.
	For input parameters, sockfd are the socket fd of data connection, and dir is the string including sub-directory and file list under current directory.

	The return value is 0 if sending OK, otherwise -1 is returned. */

	/* To get sub-directory and file list, you can execute shell command by using 
system( ) or use functions as opendir( ) and readdir( )  */
int send_list(int sockfd, char *dir)
{
	char buf[128];
	int len = sizeof(buf);
	DIR *dirptr = NULL;
	int flag = -1; //Used to check whether directory is read successfully
	struct dirent *entry;
	int off;
	if((dirptr = opendir(dir)) == NULL)
		printf("opendir() failed\n");
	while (entry = readdir(dirptr)) {
		char *filename = entry->d_name;
		struct stat st;
		char mode[] = "----------";
		struct passwd *pwd;
		struct group *grp;
		struct tm *ptm;
		char timebuf[128];
		int timelen;
		memset(buf, 0, sizeof(buf));
		off = 0;
		sprintf(buf, "%s", filename);
		if (strncmp(buf, ".", 1) != 0 && strncmp(buf, "..", 2) != 0) {
			if (stat(filename, &st) < 0)
				printf("stat() failed\n");
			//mode
			if (S_ISDIR(st.st_mode))
				mode[0] = 'd';
			if (st.st_mode & S_IRUSR)
				mode[1] = 'r';
			if (st.st_mode & S_IWUSR)
				mode[2] = 'w';
			if (st.st_mode & S_IXUSR)
				mode[3] = 'x';
			if (st.st_mode & S_IRGRP)
				mode[4] = 'r';
			if (st.st_mode & S_IWGRP)
				mode[5] = 'w';
			if (st.st_mode & S_IXGRP)
				mode[6] = 'x';
			if (st.st_mode & S_IROTH)
				mode[7] = 'r';
			if (st.st_mode & S_IWOTH)
				mode[8] = 'w';
			if (st.st_mode & S_IXOTH)
				mode[9] = 'x';
			mode[10] = '\0';
			off += snprintf(buf + off, len - off, "%s  ", mode);
			// hard link number, this field is nonsense for ftp
			off += snprintf(buf + off, len - off, "%d  ", 1);
			//size
			off += snprintf(buf + off, len - off, "%dBytes  ", getFileSize(filename));
			//mtime
			ptm = localtime(&st.st_mtime);
			if (ptm && (timelen = strftime(timebuf, sizeof(timebuf), "%b %d %H:%S  ", ptm)) > 0)
				timebuf[timelen] = '\0';
			off += snprintf(buf + off, len - off, "%-15s ", timebuf);
			strcat(buf, entry->d_name);
			strcat(buf, "\r\n");
			write(sockfd, buf, strlen(buf));
			flag = 0;
		}
	}
	closedir(dirptr);
	return flag;
}

//Find out file size
int getFileSize(char * strFileName)   
{  
    FILE * fp = fopen(strFileName, "r");  
    fseek(fp, 0L, SEEK_END);  
    int size = ftell(fp);  
    fclose(fp);  
    return size;  
}   

/*Find out the server IP. Success return 0. Error return -1.*/
int getlocalip(char* outip)
{
	int i=0;
	int sockfd;
	struct ifconf ifconf;
	char buf[512];
	struct ifreq *ifreq;
	char* ip;
	ifconf.ifc_len = 512;
	ifconf.ifc_buf = buf;
	if((sockfd = socket(AF_INET, SOCK_DGRAM, 0))<0){
		return -1;
	}
	ioctl(sockfd, SIOCGIFCONF, &ifconf);    
	close(sockfd);

	ifreq = (struct ifreq*)buf;
	for(i=(ifconf.ifc_len/sizeof(struct ifreq)); i>0; i--){
		ip = inet_ntoa(((struct sockaddr_in*) &(ifreq->ifr_addr))->sin_addr);
		if(strcmp(ip,"127.0.0.1")==0)
		{
			ifreq++;
			continue;
		}
		strcpy(outip,ip);
		return 0;
	}
	return -1;
}

