#include <linux/kernel.h>
#include <linux/input-event-codes.h>
#include <linux/string.h>

/* ================== State / event definitions ================== */
typedef enum {
    ST_IDLE = 0,
    ST_ACTIVE,
    ST_WAIT_ACTIVE_LEAVE,
    ST_ONESHOT,
    ST_HOLD,
    ST_LOCKED,
    ST_LOCKED_WAIT_LEAVE,
    ST_LOCKED_LEAVE,
    ST_COUNT
} State;

typedef enum {
    EV_KEY_PRES = 0,
    EV_KEY_RELE,
    EV_OTHER_KEY_RELE,
    EV_TIMEOUT_300,
    EV_COUNT
} Event;

/* ================== Device instance (one independent key/LED device) ================== */
typedef struct {
    int   id;          // Device identifier used in diagnostic output
    State state;       // Current state
    /* Per-device timer counters or private data can be added here later. */
	int layer_active;
	void *ctx;
} tca8418_t;

/* ================== Action functions operating on the referenced device ================== */
static void enable_layer(tca8418_t *me)         { me->layer_active = 1; }
static void disable_layer(tca8418_t *me)        { me->layer_active = 0; }
void emit_key_pres_status(tca8418_t *me) ;
void emit_key_release_status(tca8418_t *me) ;
void led_ON(tca8418_t *me)               ;
void led_OFF(tca8418_t *me)              ;
void led_BLINK(tca8418_t *me)            ;
void led_FAST(tca8418_t *me)             ;

/* ================== State entry/exit actions (use the me pointer) ================== */
static void state_entry(tca8418_t *me, State s)
{
    switch (s) {
    case ST_ACTIVE:
        enable_layer(me);
        emit_key_pres_status(me);
        led_ON(me);
        break;
    case ST_ONESHOT:
        led_BLINK(me);
        break;
    case ST_LOCKED:
        led_FAST(me);
        break;
    default:
        break;
    }
}

static void state_exit(tca8418_t *me, State s)
{
    switch (s) {
    case ST_ONESHOT:
    case ST_HOLD:
    case ST_LOCKED_LEAVE:
        disable_layer(me);
        emit_key_release_status(me);
        led_OFF(me);
        break;
    default:
        break;
    }
}

/* ================== State transition ================== */
static void transition(tca8418_t *me, State next)
{
    // printf("[Dev%d] Transition: %s -> %s\n", me->id, state_name[me->state], state_name[next]);
    state_exit(me, me->state);
    me->state = next;
    state_entry(me, me->state);
}

/* ================== Event dispatch (core FSM logic for the referenced device) ================== */
static void tca8418_dispatch(tca8418_t * const me, Event e)
{
    // printf("\n>>> [Dev%d] Event: %s (current state: %s)\n",
    //        me->id, event_name[e], state_name[me->state]);

    switch (me->state) {

    case ST_IDLE:
        if (e == EV_KEY_PRES) {
            transition(me, ST_ACTIVE);
        }
        break;

    case ST_ACTIVE:
        if (e == EV_KEY_RELE) {
            transition(me, ST_WAIT_ACTIVE_LEAVE);
        } else if (e == EV_TIMEOUT_300) {
            transition(me, ST_HOLD);
        }
        break;

    case ST_WAIT_ACTIVE_LEAVE:
        if (e == EV_TIMEOUT_300) {
            transition(me, ST_ONESHOT);
        } else if (e == EV_KEY_PRES) {
            transition(me, ST_LOCKED);
        }
        break;

    case ST_ONESHOT:
        if (e == EV_OTHER_KEY_RELE) {
            transition(me, ST_IDLE);
        }
        break;

    case ST_HOLD:
        if (e == EV_KEY_RELE) {
            transition(me, ST_IDLE);
        }
        break;

    case ST_LOCKED:
        if (e == EV_KEY_RELE) {
            transition(me, ST_LOCKED_WAIT_LEAVE);
        }
        break;

    case ST_LOCKED_WAIT_LEAVE:
        if (e == EV_KEY_PRES) {
            transition(me, ST_LOCKED_LEAVE);
        }
        break;

    case ST_LOCKED_LEAVE:
        if (e == EV_KEY_RELE) {
            transition(me, ST_IDLE);
        }
        break;

    default:
        break;
    }
}

/* ================== Initialization ================== */
static void tca8418_ctor(tca8418_t *me, int id)
{
    me->id = id;
    me->state = ST_IDLE;
    // printf("[Dev%d] Initialization complete, initial state: %s\n", id, state_name[me->state]);
}

/* ================== main: create and drive three independent device instances ================== */
// #define NUM_DEVICES 3

// int main(void)
// {
//     tca8418_t devices[NUM_DEVICES];
//     int i;

//     printf("========== Multi-instance key FSM simulator (%d independent devices) ==========\n", NUM_DEVICES);

//     for (i = 0; i < NUM_DEVICES; ++i) {
//         tca8418_ctor(&devices[i], i);
//     }

//     char buf[32];

//     while (1) {
//         printf("\n----------------------------------------\n");
//         for (i = 0; i < NUM_DEVICES; ++i) {
//             printf("Dev%d current state: %s\n", i, state_name[devices[i].state]);
//         }
//         printf("----------------------------------------\n");
//         printf("Enter [device ID (0-%d)] [event ID (0-%d)]; for example, '0 2' sends KEY_RELE to Dev0\n",
//                NUM_DEVICES - 1, EV_COUNT - 1);
//         printf("Event IDs: 0=ON_KEY_PRES 1=KEY_PRES 2=KEY_RELE 3=OTHER_KEY_RELE 4=TIMEOUT_300\n");
//         printf("Enter q to quit\n> ");

//         if (!fgets(buf, sizeof(buf), stdin)) {
//             break;
//         }
//         if (buf[0] == 'q' || buf[0] == 'Q') {
//             printf("Exiting.\n");
//             break;
//         }

//         int dev_idx, ev_idx;
//         if (sscanf(buf, "%d %d", &dev_idx, &ev_idx) != 2) {
//             printf("Invalid input format; please try again.\n");
//             continue;
//         }

//         if (dev_idx < 0 || dev_idx >= NUM_DEVICES) {
//             printf("Invalid device ID.\n");
//             continue;
//         }
//         if (ev_idx < 0 || ev_idx >= EV_COUNT) {
//             printf("Invalid event ID.\n");
//             continue;
//         }

//         /* Key point: pass the corresponding device pointer to the FSM dispatcher;
//            each device maintains completely independent state and actions. */
//         tca8418_dispatch(&devices[dev_idx], (Event)ev_idx);
//     }

//     return 0;
// }
