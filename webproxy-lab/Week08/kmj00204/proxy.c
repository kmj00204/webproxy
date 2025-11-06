#include "csapp.h" // CS:APP에서 제공하는 네트워크/IO 유틸 함수들의 헤더

#define MAX_CACHE_SIZE 1049000 // 캐시 가능한 최대 용량 (사용하지 않았지만 과제용 상수)
#define MAX_OBJECT_SIZE 102400 // 한 객체의 최대 크기 (역시 캐시용 상수)

/* 웹 브라우저의 User-Agent 헤더를 그대로 보내기 위한 상수 문자열 */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

/* 함수 선언 */
void doit(int fd);                                                                                  // 클라이언트 요청을 처리하는 함수
void read_requesthdrs(rio_t *rp);                                                                   // 요청 헤더를 읽는 함수 (현재는 거의 사용 안 됨)
int parse_uri(char *uri, char *hostname, char *path, int *port);                                    // URI 파싱
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio); // 서버로 보낼 HTTP 헤더 구성
int connect_endServer(char *hostname, int port);                                                    // 원격 서버와 연결하는 함수

/* 메인 함수: 프록시 서버의 진입점 */
int main(int argc, char **argv)
{
    int listenfd, connfd;                  // 리스닝 소켓과 연결 소켓
    char hostname[MAXLINE], port[MAXLINE]; // 클라이언트의 호스트 이름과 포트
    socklen_t clientlen;                   // 클라이언트 주소 구조체의 크기
    struct sockaddr_storage clientaddr;    // 클라이언트의 주소 정보

    // 포트 번호를 인자로 안 받으면 사용법 출력 후 종료
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    // 지정된 포트로 리스닝 소켓 생성
    listenfd = Open_listenfd(argv[1]);

    while (1)
    {
        // 클라이언트 연결 수락 준비
        clientlen = sizeof(clientaddr);

        // 클라이언트 연결 수락 → 새로운 소켓(connfd) 생성
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

        // 클라이언트의 주소를 호스트 이름, 포트로 변환
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        // 하나의 요청을 처리 (HTTP 트랜잭션)
        doit(connfd);

        // 연결 종료
        Close(connfd);
    }
    return 0;
}

/* doit: 하나의 HTTP 요청/응답을 처리하는 핵심 함수 */
void doit(int fd)
{
    int end_server_fd;                                                  // 원 서버(웹 서버)와 통신할 소켓
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE]; // 요청 첫 줄 저장용
    char hostname[MAXLINE], path[MAXLINE];                              // 요청 URI에서 파싱된 호스트/경로
    int port;                                                           // 요청 URI에서 파싱된 포트
    rio_t rio, server_rio;                                              // Robust I/O 구조체

    // 클라이언트 소켓을 rio 구조체로 초기화
    Rio_readinitb(&rio, fd);

    // 클라이언트로부터 요청 라인(예: "GET http://... HTTP/1.1")을 읽음
    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return; // EOF면 종료

    printf("Request headers:\n%s", buf);

    // 요청 라인에서 메서드, URI, 버전을 파싱
    sscanf(buf, "%s %s %s", method, uri, version);

    // 프록시는 GET만 지원 (POST, PUT 등은 처리 안 함)
    if (strcasecmp(method, "GET"))
    {
        printf("Proxy does not implement the method\n");
        return;
    }

    // URI를 파싱해서 hostname, path, port 추출
    parse_uri(uri, hostname, path, &port);

    // 서버로 보낼 HTTP 요청 헤더를 구성
    char http_header[MAXLINE];
    build_http_header(http_header, hostname, path, port, &rio);

    // 원 서버(엔드 서버)에 연결
    end_server_fd = connect_endServer(hostname, port);
    if (end_server_fd < 0)
    {
        printf("Connection failed\n");
        return;
    }

    // 원 서버와의 통신용 rio 구조체 초기화
    Rio_readinitb(&server_rio, end_server_fd);

    // 원 서버로 HTTP 요청 헤더 전송
    Rio_writen(end_server_fd, http_header, strlen(http_header));

    // 원 서버로부터 응답을 읽고, 클라이언트에게 그대로 전달
    size_t n;
    while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0)
    {
        printf("Proxy received %d bytes, then send\n", (int)n);
        Rio_writen(fd, buf, n);
    }

    // 서버 소켓 닫기
    Close(end_server_fd);
}

/* parse_uri: URI를 호스트 이름, 경로, 포트로 나누는 함수
   예: "http://example.com:8080/index.html" → hostname=example.com, port=8080, path=/index.html */
int parse_uri(char *uri, char *hostname, char *path, int *port)
{
    *port = 80; // 기본 포트는 80

    // "http://" 이후 위치를 찾음
    char *pos = strstr(uri, "//");
    pos = (pos != NULL) ? pos + 2 : uri; // "//" 다음부터 시작

    // 포트 번호가 명시되어 있는지 확인
    char *portpos = strchr(pos, ':');
    if (portpos)
    {
        *portpos = '\0';                         // ':' 위치를 문자열 끝으로 자름
        sscanf(pos, "%s", hostname);             // ':' 앞까지 hostname으로 저장
        sscanf(portpos + 1, "%d%s", port, path); // ':' 뒤에서 포트와 path 추출
    }
    else
    {
        // 포트가 없으면 '/' 기준으로 경로 확인
        char *pathpos = strchr(pos, '/');
        if (pathpos)
        {
            *pathpos = '\0';
            sscanf(pos, "%s", hostname); // '/' 앞부분이 호스트
            *path = '\0';
            strcat(path, pathpos); // '/'부터 끝까지 경로
        }
        else
        {
            // 경로가 없으면 기본값 "/"로 설정
            sscanf(pos, "%s", hostname);
            strcpy(path, "/");
        }
    }
    return 0;
}

/* build_http_header: 원 서버로 보낼 완전한 HTTP 요청 헤더 구성 */
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio)
{
    char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];

    // 요청 라인: GET /path HTTP/1.0
    sprintf(request_hdr, "GET %s HTTP/1.0\r\n", path);

    // Host 헤더
    sprintf(host_hdr, "Host: %s\r\n", hostname);

    // User-Agent, Connection, Proxy-Connection 헤더
    sprintf(other_hdr, "%s"
                       "Connection: close\r\n"
                       "Proxy-Connection: close\r\n"
                       "%s",
            user_agent_hdr, "\r\n");

    // 최종 헤더 문자열 합치기
    sprintf(http_header, "%s%s%s", request_hdr, host_hdr, other_hdr);
}

/* connect_endServer: 주어진 hostname과 port로 원격 서버에 TCP 연결 생성 */
int connect_endServer(char *hostname, int port)
{
    char portStr[100];
    sprintf(portStr, "%d", port);            // 정수 포트를 문자열로 변환
    return Open_clientfd(hostname, portStr); // 클라이언트 소켓 생성
}

/* read_requesthdrs: 클라이언트 요청의 나머지 헤더들을 읽고 무시 */
void read_requesthdrs(rio_t *rp)
{
    char buf[MAXLINE];
    Rio_readlineb(rp, buf, MAXLINE);
    while (strcmp(buf, "\r\n")) // 빈 줄(헤더의 끝)을 만날 때까지
    {
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf);
    }
}
#include "csapp.h" // CS:APP에서 제공하는 네트워크/IO 유틸 함수들의 헤더

#define MAX_CACHE_SIZE 1049000 // 캐시 가능한 최대 용량 (사용하지 않았지만 과제용 상수)
#define MAX_OBJECT_SIZE 102400 // 한 객체의 최대 크기 (역시 캐시용 상수)

/* 웹 브라우저의 User-Agent 헤더를 그대로 보내기 위한 상수 문자열 */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

/* 함수 선언 */
void doit(int fd);                                                                                  // 클라이언트 요청을 처리하는 함수
void read_requesthdrs(rio_t *rp);                                                                   // 요청 헤더를 읽는 함수 (현재는 거의 사용 안 됨)
int parse_uri(char *uri, char *hostname, char *path, int *port);                                    // URI 파싱
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio); // 서버로 보낼 HTTP 헤더 구성
int connect_endServer(char *hostname, int port);                                                    // 원격 서버와 연결하는 함수

/* 메인 함수: 프록시 서버의 진입점 */
int main(int argc, char **argv)
{
    int listenfd, connfd;                  // 리스닝 소켓과 연결 소켓
    char hostname[MAXLINE], port[MAXLINE]; // 클라이언트의 호스트 이름과 포트
    socklen_t clientlen;                   // 클라이언트 주소 구조체의 크기
    struct sockaddr_storage clientaddr;    // 클라이언트의 주소 정보

    // 포트 번호를 인자로 안 받으면 사용법 출력 후 종료
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    // 지정된 포트로 리스닝 소켓 생성
    listenfd = Open_listenfd(argv[1]);

    while (1)
    {
        // 클라이언트 연결 수락 준비
        clientlen = sizeof(clientaddr);

        // 클라이언트 연결 수락 → 새로운 소켓(connfd) 생성
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

        // 클라이언트의 주소를 호스트 이름, 포트로 변환
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        // 하나의 요청을 처리 (HTTP 트랜잭션)
        doit(connfd);

        // 연결 종료
        Close(connfd);
    }
    return 0;
}

/* doit: 하나의 HTTP 요청/응답을 처리하는 핵심 함수 */
void doit(int fd)
{
    int end_server_fd;                                                  // 원 서버(웹 서버)와 통신할 소켓
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE]; // 요청 첫 줄 저장용
    char hostname[MAXLINE], path[MAXLINE];                              // 요청 URI에서 파싱된 호스트/경로
    int port;                                                           // 요청 URI에서 파싱된 포트
    rio_t rio, server_rio;                                              // Robust I/O 구조체

    // 클라이언트 소켓을 rio 구조체로 초기화
    Rio_readinitb(&rio, fd);

    // 클라이언트로부터 요청 라인(예: "GET http://... HTTP/1.1")을 읽음
    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return; // EOF면 종료

    printf("Request headers:\n%s", buf);

    // 요청 라인에서 메서드, URI, 버전을 파싱
    sscanf(buf, "%s %s %s", method, uri, version);

    // 프록시는 GET만 지원 (POST, PUT 등은 처리 안 함)
    if (strcasecmp(method, "GET"))
    {
        printf("Proxy does not implement the method\n");
        return;
    }

    // URI를 파싱해서 hostname, path, port 추출
    parse_uri(uri, hostname, path, &port);

    // 서버로 보낼 HTTP 요청 헤더를 구성
    char http_header[MAXLINE];
    build_http_header(http_header, hostname, path, port, &rio);

    // 원 서버(엔드 서버)에 연결
    end_server_fd = connect_endServer(hostname, port);
    if (end_server_fd < 0)
    {
        printf("Connection failed\n");
        return;
    }

    // 원 서버와의 통신용 rio 구조체 초기화
    Rio_readinitb(&server_rio, end_server_fd);

    // 원 서버로 HTTP 요청 헤더 전송
    Rio_writen(end_server_fd, http_header, strlen(http_header));

    // 원 서버로부터 응답을 읽고, 클라이언트에게 그대로 전달
    size_t n;
    while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0)
    {
        printf("Proxy received %d bytes, then send\n", (int)n);
        Rio_writen(fd, buf, n);
    }

    // 서버 소켓 닫기
    Close(end_server_fd);
}

/* parse_uri: URI를 호스트 이름, 경로, 포트로 나누는 함수
   예: "http://example.com:8080/index.html" → hostname=example.com, port=8080, path=/index.html */
int parse_uri(char *uri, char *hostname, char *path, int *port)
{
    *port = 80; // 기본 포트는 80

    // "http://" 이후 위치를 찾음
    char *pos = strstr(uri, "//");
    pos = (pos != NULL) ? pos + 2 : uri; // "//" 다음부터 시작

    // 포트 번호가 명시되어 있는지 확인
    char *portpos = strchr(pos, ':');
    if (portpos)
    {
        *portpos = '\0';                         // ':' 위치를 문자열 끝으로 자름
        sscanf(pos, "%s", hostname);             // ':' 앞까지 hostname으로 저장
        sscanf(portpos + 1, "%d%s", port, path); // ':' 뒤에서 포트와 path 추출
    }
    else
    {
        // 포트가 없으면 '/' 기준으로 경로 확인
        char *pathpos = strchr(pos, '/');
        if (pathpos)
        {
            *pathpos = '\0';
            sscanf(pos, "%s", hostname); // '/' 앞부분이 호스트
            *path = '\0';
            strcat(path, pathpos); // '/'부터 끝까지 경로
        }
        else
        {
            // 경로가 없으면 기본값 "/"로 설정
            sscanf(pos, "%s", hostname);
            strcpy(path, "/");
        }
    }
    return 0;
}

/* build_http_header: 원 서버로 보낼 완전한 HTTP 요청 헤더 구성 */
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio)
{
    char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];

    // 요청 라인: GET /path HTTP/1.0
    sprintf(request_hdr, "GET %s HTTP/1.0\r\n", path);

    // Host 헤더
    sprintf(host_hdr, "Host: %s\r\n", hostname);

    // User-Agent, Connection, Proxy-Connection 헤더
    sprintf(other_hdr, "%s"
                       "Connection: close\r\n"
                       "Proxy-Connection: close\r\n"
                       "%s",
            user_agent_hdr, "\r\n");

    // 최종 헤더 문자열 합치기
    sprintf(http_header, "%s%s%s", request_hdr, host_hdr, other_hdr);
}

/* connect_endServer: 주어진 hostname과 port로 원격 서버에 TCP 연결 생성 */
int connect_endServer(char *hostname, int port)
{
    char portStr[100];
    sprintf(portStr, "%d", port);            // 정수 포트를 문자열로 변환
    return Open_clientfd(hostname, portStr); // 클라이언트 소켓 생성
}

/* read_requesthdrs: 클라이언트 요청의 나머지 헤더들을 읽고 무시 */
void read_requesthdrs(rio_t *rp)
{
    char buf[MAXLINE];
    Rio_readlineb(rp, buf, MAXLINE);
    while (strcmp(buf, "\r\n")) // 빈 줄(헤더의 끝)을 만날 때까지
    {
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf);
    }
}
