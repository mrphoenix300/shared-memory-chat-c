#include "common.h"
#include <sys/file.h>
#include <time.h>
#include <stdatomic.h>
#include <poll.h>

#define SHM_READY_MAGIC 0x43484154u

// global variables
SharedSegment *shm_ptr;      
atomic_int running = 1;
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
    pthread_mutex_lock(&myRoom->mutex);
    local_seq = myRoom->global_seq_count;
    pthread_mutex_unlock(&myRoom->mutex);

    while (atomic_load(&running)) {
        pthread_mutex_lock(&myRoom->mutex); // enters critical section

        // wait for a new message to arrive
        while (local_seq >= myRoom->global_seq_count && atomic_load(&running)) {
            pthread_cond_wait(&myRoom->new_msg_cond, &myRoom->mutex);
        }

        if (!atomic_load(&running)) {
            pthread_mutex_unlock(&myRoom->mutex);
            break;
        }

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
                atomic_store(&running, 0); 
                
                if (msg->readers_left > 0) {
                    msg->readers_left--; // has been read
                    if (msg->readers_left == 0) {
                        pthread_cond_signal(&myRoom->space_cond); // notify writer
                    }
                }
                
                // wake up others just in case they are stuck waiting
                pthread_cond_broadcast(&myRoom->new_msg_cond);
                pthread_mutex_unlock(&myRoom->mutex);
                pthread_exit(NULL);
            }

            if (msg->sender_pid != my_pid) {
                printf("\r[User %d]: %s\n", msg->sender_pid, msg->payload);
                printf("You (Room %d): ", current_room_id); 
                fflush(stdout);
            }

            if (msg->readers_left > 0) {
                msg->readers_left--; // has been read
                if (msg->readers_left == 0) {
                    pthread_cond_signal(&myRoom->space_cond); // notify writer that space is free
                }
            }
            
            local_seq++; 
        } else {
            local_seq++;
        }
        
        pthread_mutex_unlock(&myRoom->mutex); // leaves critical section
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int shm_fd;
    shm_ptr = NULL;
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
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);

    if (shm_fd != -1) {
        // Hold the advisory file lock until every shared mutex is ready.
        if (flock(shm_fd, LOCK_EX) == -1) { perror("flock creator"); exit(1); }
        printf("Created Shared Memory Segment.\n");
        
        if (ftruncate(shm_fd, sizeof(SharedSegment)) == -1) { perror("ftruncate"); exit(1); }
        
        shm_ptr = mmap(NULL, sizeof(SharedSegment), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_ptr == MAP_FAILED) { perror("mmap"); exit(1); }

        // setup attributes for inter-process sharing
        pthread_mutexattr_t mattr;
        pthread_condattr_t cattr;
        
        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
        
        pthread_condattr_init(&cattr);
        pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);

        pthread_mutex_init(&shm_ptr->lifecycle_mutex, &mattr);
        shm_ptr->total_participants = 0;

        // initialization of all chat rooms
        for(int i = 0; i < MAX_ROOMS; i++) {
            pthread_mutex_init(&shm_ptr->rooms[i].mutex, &mattr);
            pthread_cond_init(&shm_ptr->rooms[i].new_msg_cond, &cattr);
            pthread_cond_init(&shm_ptr->rooms[i].space_cond, &cattr);
            
            shm_ptr->rooms[i].active_participants = 0;
            shm_ptr->rooms[i].head = 0;
            shm_ptr->rooms[i].global_seq_count = 0;
            memset(shm_ptr->rooms[i].messages, 0, sizeof(shm_ptr->rooms[i].messages));
        }

        // cleanup attributes
        pthread_mutexattr_destroy(&mattr);
        pthread_condattr_destroy(&cattr);

        // Publish readiness only after initialization is complete.
        shm_ptr->initialized_magic = SHM_READY_MAGIC;
        if (flock(shm_fd, LOCK_UN) == -1) { perror("flock unlock"); exit(1); }

        // enter the chat room I have selected
        pthread_mutex_lock(&shm_ptr->lifecycle_mutex);
        pthread_mutex_lock(&shm_ptr->rooms[current_room_id].mutex);
        shm_ptr->rooms[current_room_id].active_participants++;
        shm_ptr->total_participants++;
        pthread_mutex_unlock(&shm_ptr->rooms[current_room_id].mutex);
        pthread_mutex_unlock(&shm_ptr->lifecycle_mutex);
        
        printf("Initialized %d rooms. Joined Room %d as Creator.\n", MAX_ROOMS, current_room_id);

    } else {
        if (errno == EEXIST) {
            // joiner
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0600);
            if (shm_fd == -1) { perror("shm_open existing"); exit(1); }

            // Wait for creator initialization without touching uninitialized mutexes.
            int ready = 0;
            for (int attempt = 0; attempt < 40; attempt++) {
                if (flock(shm_fd, LOCK_EX) == -1) { perror("flock joiner"); close(shm_fd); return 1; }

                struct stat shm_stat;
                if (fstat(shm_fd, &shm_stat) == -1) {
                    perror("fstat");
                    flock(shm_fd, LOCK_UN);
                    close(shm_fd);
                    return 1;
                }

                if (shm_stat.st_size == (off_t)sizeof(SharedSegment)) {
                    if (!shm_ptr || shm_ptr == MAP_FAILED) {
                        shm_ptr = mmap(NULL, sizeof(SharedSegment), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
                        if (shm_ptr == MAP_FAILED) { perror("mmap"); flock(shm_fd, LOCK_UN); close(shm_fd); return 1; }
                    }
                    if (shm_ptr->initialized_magic == SHM_READY_MAGIC) {
                        ready = 1;
                        flock(shm_fd, LOCK_UN);
                        break;
                    }
                } else if (shm_stat.st_size != 0) {
                    fprintf(stderr, "Incompatible shared memory size.\n");
                    flock(shm_fd, LOCK_UN);
                    close(shm_fd);
                    return 1;
                }

                flock(shm_fd, LOCK_UN);
                struct timespec pause_time = {.tv_sec = 0, .tv_nsec = 50000000};
                nanosleep(&pause_time, NULL);
            }
            if (!ready) {
                fprintf(stderr, "Shared memory initialization timed out.\n");
                if (shm_ptr && shm_ptr != MAP_FAILED) munmap(shm_ptr, sizeof(SharedSegment));
                close(shm_fd);
                return 1;
            }

            // enter the chat room I have selected
            pthread_mutex_lock(&shm_ptr->lifecycle_mutex);
            pthread_mutex_lock(&shm_ptr->rooms[current_room_id].mutex);
            shm_ptr->rooms[current_room_id].active_participants++;
            shm_ptr->total_participants++;
            pthread_mutex_unlock(&shm_ptr->rooms[current_room_id].mutex);
            pthread_mutex_unlock(&shm_ptr->lifecycle_mutex);  
            
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

    while (atomic_load(&running)) {
        printf("You (Room %d): ", current_room_id);
        fflush(stdout);

        // Check for peer shutdown while waiting for keyboard input.
        // Unlike fgets alone, poll can wake periodically to observe running.
        struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
        int poll_result = poll(&input, 1, 100);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            perror("poll stdin");
            break;
        }
        if (!atomic_load(&running)) break;
        if (poll_result == 0) continue;
        if (input.revents & (POLLERR | POLLNVAL)) break;
        if (!(input.revents & (POLLIN | POLLHUP))) continue;
        if (fgets(buffer, MSG_SIZE, stdin) == NULL) break;
        buffer[strcspn(buffer, "\n")] = 0; 

        if (strlen(buffer) == 0) continue;

        pthread_mutex_lock(&myRoom->mutex); // enters critical section

        if (!atomic_load(&running)) {
            pthread_mutex_unlock(&myRoom->mutex);
            break;
        }

        // check if full
        int idx = myRoom->head;
       // check if full and wait for readers
        while (myRoom->messages[idx].readers_left > 0 && atomic_load(&running)) {
            // blocks until space_cond is signaled by a reader
            pthread_cond_wait(&myRoom->space_cond, &myRoom->mutex);
        }

        if (!atomic_load(&running)) {
            pthread_mutex_unlock(&myRoom->mutex);
            break;
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
            atomic_store(&running, 0);
            printf("[System]: You sent TERMINATE. Exiting...\n");
        }

        // wake up ALL waiting readers simultaneously
        pthread_cond_broadcast(&myRoom->new_msg_cond);

        pthread_mutex_unlock(&myRoom->mutex); // leaves critical section
    }

    // Tell the reader to stop and wake it even if no new messages arrive.
    // Avoid pthread_cancel: cancellation in pthread_cond_wait can leave the
    // process-shared room mutex locked during stack unwinding.
    atomic_store(&running, 0);
    pthread_mutex_lock(&myRoom->mutex);
    pthread_cond_broadcast(&myRoom->new_msg_cond);
    pthread_cond_broadcast(&myRoom->space_cond);
    pthread_mutex_unlock(&myRoom->mutex);
    pthread_join(reader_tid, NULL);

    // Serialize joins and leaves across all rooms. The final participant
    // unlinks while holding the lifecycle lock, preventing concurrent joins.
    pthread_mutex_lock(&shm_ptr->lifecycle_mutex);
    pthread_mutex_lock(&myRoom->mutex);
    myRoom->active_participants--;
    pthread_mutex_unlock(&myRoom->mutex);
    shm_ptr->total_participants--;
    int all_empty = (shm_ptr->total_participants == 0);
    if (all_empty && shm_unlink(SHM_NAME) == -1) {
        perror("shm_unlink");
    }
    pthread_mutex_unlock(&shm_ptr->lifecycle_mutex);

    munmap(shm_ptr, sizeof(SharedSegment));
    close(shm_fd);

    if (all_empty) {
        printf("Last user of the ENTIRE system left. Removing Shared Memory.\n");
    } else {
        printf("Exited Room %d. System remains active for other rooms.\n", current_room_id);
    }

    return 0;
}