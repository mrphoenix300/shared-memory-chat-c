#include "common.h"

// global variables
SharedSegment *shm_ptr;      
volatile int running = 1;
int my_pid;                 
int current_room_id = 0; // which chat room we are in

// helper for checking if all rooms are empty to clear the memory
int are_all_rooms_empty() {
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (shm_ptr->rooms[i].active_participants > 0) {
            return 0; // is not empty
        }
    }
    return 1; // is empty
}

// reader thread
void *reader(void *arg) {
    int local_seq;
    
    // pointer to the specific chat room for simplicity
    ChatRoom *myRoom = &shm_ptr->rooms[current_room_id];

    // initial synchronization
    sem_wait(&myRoom->mutex);
    local_seq = myRoom->global_seq_count;
    sem_post(&myRoom->mutex);

    while (running) {
        usleep(100000); // 100ms sleep for cpu save

        sem_wait(&myRoom->mutex); // enters critical section

        if (local_seq < myRoom->global_seq_count) {
            
            // we search for the message in the buffer of that room
            int target_id = local_seq + 1;
            int found_idx = -1;
            
            for(int i=0; i<MAX_MSGS; i++) {
                if(myRoom->messages[i].id == target_id) {
                    found_idx = i;
                    break;
                }
            }

            if (found_idx != -1) {
                Message *msg = &myRoom->messages[found_idx];

                if (strcmp(msg->payload, "TERMINATE") == 0) { // terminates when the user enter "TERMINATE" message
                    printf("\n[System]: Received TERMINATE signal in Room %d. Shutting down...\n", current_room_id);
                    running = 0; 
                    if (msg->readers_left > 0) msg->readers_left--; // has been read
                    sem_post(&myRoom->mutex);
                    pthread_exit(NULL);
                }

                if (msg->sender_pid != my_pid) {
                    printf("\r[User %d]: %s\n", msg->sender_pid, msg->payload);
                    printf("You (Room %d): ", current_room_id); 
                    fflush(stdout);
                }

                if (msg->readers_left > 0) {
                    msg->readers_left--; // has been read
                }
                
                local_seq++; 
            } else {
                local_seq++;
            }
        }
        sem_post(&myRoom->mutex); // leaves critical section
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int shm_fd;
    pthread_t reader_tid;
    my_pid = getpid();
    
    // chat room selection
    printf("Available Rooms: 0 to %d\n", MAX_ROOMS - 1);
    printf("Enter Room ID to join: ");
    if (scanf("%d", &current_room_id) != 1) return 1;
    getchar(); // eat newline

    if (current_room_id < 0 || current_room_id >= MAX_ROOMS) {
        printf("Invalid Room ID!\n");
        return 1;
    }

    // οpening shared memory (one for all)
    // we try to create the memory (O_CREAT)
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);

    if (shm_fd != -1) {
        // creator 
        printf("Created Shared Memory Segment.\n");
        
        if (ftruncate(shm_fd, sizeof(SharedSegment)) == -1) { perror("ftruncate"); exit(1); }
        
        shm_ptr = mmap(NULL, sizeof(SharedSegment), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_ptr == MAP_FAILED) { perror("mmap"); exit(1); }

        // initialization of all chat rooms
        for(int i = 0; i < MAX_ROOMS; i++) {
            sem_init(&shm_ptr->rooms[i].mutex, 1, 1);
            shm_ptr->rooms[i].active_participants = 0;
            shm_ptr->rooms[i].head = 0;
            shm_ptr->rooms[i].global_seq_count = 0;
            memset(shm_ptr->rooms[i].messages, 0, sizeof(shm_ptr->rooms[i].messages));
        }

        // enter the chat room I have selected
        sem_wait(&shm_ptr->rooms[current_room_id].mutex);
        shm_ptr->rooms[current_room_id].active_participants++;
        sem_post(&shm_ptr->rooms[current_room_id].mutex);
        
        printf("Initialized %d rooms. Joined Room %d as Creator.\n", MAX_ROOMS, current_room_id);

    } else {
        if (errno == EEXIST) {
            // joiner
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
            if (shm_fd == -1) { perror("shm_open existing"); exit(1); }

            shm_ptr = mmap(NULL, sizeof(SharedSegment), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
            if (shm_ptr == MAP_FAILED) { perror("mmap"); exit(1); }

            // enter the chat room I have selected
            sem_wait(&shm_ptr->rooms[current_room_id].mutex);
            shm_ptr->rooms[current_room_id].active_participants++;
            sem_post(&shm_ptr->rooms[current_room_id].mutex);
            
            printf("Joined existing Shared Memory. Entered Room %d.\n", current_room_id);

        } else {
            perror("shm_open fail");
            exit(1);
        }
    }
    
    // spawn reader thread
    if (pthread_create(&reader_tid, NULL, reader, NULL) != 0) {
        perror("pthread_create"); exit(1);
    }
    
    // main writer loop
    char buffer[MSG_SIZE];
    ChatRoom *myRoom = &shm_ptr->rooms[current_room_id];

    printf("Chat active in Room %d. Type 'TERMINATE' to exit.\n", current_room_id);

    while (running) {
        printf("You (Room %d): ", current_room_id);
        fflush(stdout);

        if (fgets(buffer, MSG_SIZE, stdin) == NULL) break;
        buffer[strcspn(buffer, "\n")] = 0; 

        if (strlen(buffer) == 0) continue;

        sem_wait(&myRoom->mutex); // enters critical section

        if (!running) {
            sem_post(&myRoom->mutex);
            break;
        }

        // check if full
        int idx = myRoom->head;
        if (myRoom->messages[idx].readers_left > 0) {
            printf("[System]: Buffer full in Room %d. Wait for readers.\n", current_room_id);
            sem_post(&myRoom->mutex);
            continue;
        }

        // write payload
        strncpy(myRoom->messages[idx].payload, buffer, MSG_SIZE);
        myRoom->messages[idx].sender_pid = my_pid;
        myRoom->global_seq_count++;
        myRoom->messages[idx].id = myRoom->global_seq_count;
        
        // set readers count
        myRoom->messages[idx].readers_left = (myRoom->active_participants > 1) ? (myRoom->active_participants - 1) : 0;

        // move head
        myRoom->head = (myRoom->head + 1) % MAX_MSGS;

        if (strcmp(buffer, "TERMINATE") == 0) {
            running = 0;
            printf("[System]: You sent TERMINATE. Exiting...\n");
        }

        sem_post(&myRoom->mutex); // leaves critical section
    }

    // cleanup
    pthread_cancel(reader_tid); 
    pthread_join(reader_tid, NULL);

    // decrement participants only for my room
    sem_wait(&myRoom->mutex);
    myRoom->active_participants--;
    sem_post(&myRoom->mutex);

    // check cleanup
    int all_empty = are_all_rooms_empty(); // here we perform a check without a lock for simplicity, as unlink does not mind if it is done twice (the second time will fail)

    munmap(shm_ptr, sizeof(SharedSegment));
    close(shm_fd);

    if (all_empty) {
        printf("Last user of the ENTIRE system left. Removing Shared Memory.\n");
        shm_unlink(SHM_NAME);
    } else {
        printf("Exited Room %d. System remains active for other rooms.\n", current_room_id);
    }

    return 0;
}