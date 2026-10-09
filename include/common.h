#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>

#define SHM_NAME "/chat_app" // a constant name for all
#define MSG_SIZE 128
#define MAX_MSGS 100
#define MAX_ROOMS 10  // how many chat rooms supports

// struct for a message
typedef struct {
    int id;
    pid_t sender_pid;
    char payload[MSG_SIZE];
    int readers_left;
} Message;

// struct for a chat room
// every chat room have its own messages, mutex and condition variables
typedef struct {
    pthread_mutex_t mutex;           // locks only for this chat room
    pthread_cond_t new_msg_cond;     // condition for readers to wait
    pthread_cond_t space_cond;       // condition for writer to wait if buffer is full
    Message messages[MAX_MSGS];      // buffer of chat room messages
    int head;
    int global_seq_count;
    int active_participants;
} ChatRoom;

// struct for shared memory
// contains an array with all chat rooms
typedef struct {
    pthread_mutex_t lifecycle_mutex;
    int total_participants;
    ChatRoom rooms[MAX_ROOMS];
} SharedSegment;

#endif