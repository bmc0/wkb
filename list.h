#ifndef _LIST_H
#define _LIST_H

struct node {
	struct node *n;
	struct node *p;
};

#define NEW_NODE { NULL, NULL }

struct list {
	struct node *h;
	struct node *t;
};

#define NEW_LIST { NULL, NULL }

#define LIST_FOREACH(_l, _n) \
	for \
	( \
		(_n) = (void *) ((struct list *) (_l))->h; \
		(_n) != NULL; \
		(_n) = (void *) ((struct node *) (_n))->n \
	)

#define LIST_ADD_HEAD(_l, _n) do { \
	if ((_l)->h == NULL) { \
		(_l)->h = (_l)->t = (_n); \
		(_n)->n = (_n)->p = NULL; \
	} \
	else { \
		(_n)->p = NULL; \
		(_n)->n = (_l)->h; \
		(_l)->h->p = (_n); \
		(_l)->h = (_n); \
	} \
} while(0)

#define LIST_ADD_TAIL(_l, _n) do { \
	if ((_l)->t == NULL) { \
		(_l)->h = (_l)->t = (_n); \
		(_n)->n = (_n)->p = NULL; \
	} \
	else { \
		(_n)->n = NULL; \
		(_n)->p = (_l)->t; \
		(_l)->t->n = (_n); \
		(_l)->t = (_n); \
	} \
} while(0)

#define LIST_INSERT(_l, _n, _p) do { \
	if ((_p) == (_l)->t) LIST_ADD_TAIL(_l, _n); \
	else { \
		(_n)->p = (_p); \
		(_n)->n = (_p)->n; \
		(_p)->n->p = (_n); \
		(_p)->n = (_n); \
	} \
} while(0)

#define LIST_REMOVE_HEAD(_l) do { \
	if ((_l)->h != NULL) { \
		if ((_l)->h->n == NULL) { \
			(_l)->h = NULL; \
			(_l)->t = NULL; \
		} \
		else { \
			(_l)->h->n->p = NULL; \
			(_l)->h = (_l)->h->n; \
		} \
	} \
} while(0)

#define LIST_REMOVE_TAIL(_l) do { \
	if ((_l)->t != NULL) { \
		if ((_l)->t->p == NULL) { \
			(_l)->h = NULL; \
			(_l)->t = NULL; \
		} \
		else { \
			(_l)->t->p->n = NULL; \
			(_l)->t = (_l)->t->p; \
		} \
	} \
} while(0)

#define LIST_REMOVE(_l, _n) do { \
	if ((_n) == (_l)->h) LIST_REMOVE_HEAD(_l); \
	else if ((_n) == (_l)->t) LIST_REMOVE_TAIL(_l); \
	else { \
		(_n)->p->n = (_n)->n; \
		(_n)->n->p = (_n)->p; \
	} \
} while(0)

#endif
