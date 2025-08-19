#define _GNU_SOURCE
#define BUF_SIZE 10

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>   // per mkfifo

typedef struct {
    int codice;
    char *nome;
    int anno;
    int numcop;
    int *cop;
} attore;


//------------------------------------------------------------------------------------------------------------------------------
//                                                         FUNZIONI PER IL DEBUGGING
//------------------------------------------------------------------------------------------------------------------------------

void stampa_20_attori(attore **array_attori, int totale_attori) {
    int limite = (totale_attori < 10) ? totale_attori : 10;  // Stampa al massimo 20 attori
    int inizio = 169744;

    printf("--- Elenco attori (max 20) ---\n");
    for (int i = inizio; i < limite+inizio; i++) {
        attore *a = array_attori[i];
        printf("\nAttore #%d:\n", i + 1);
        printf("  Codice: %d\n", a->codice);
        printf("  Nome: %s\n", a->nome ? a->nome : "(NULL)");
        printf("  Anno: %d\n", a->anno);
        
        // Stampa i codici dei coprotagonisti (se presenti)
        printf("  Coprotagonisti [%d]: ", a->numcop);
        if (a->cop == NULL) {
            printf("NULL\n");
        } else {
            for (int j = 0; j < a->numcop; j++) {
                printf("%d", a->cop[j]);
                if (j < a->numcop - 1) printf(", ");
            }
            printf("\n");
        }
    }
    printf("\n--- Totale stampati: %d ---\n", limite);
}





//------------------------------------------------------------------------------------------------------------------------------
//                                                         DATI THREAD
//------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    attore **attori;
    int num_attori;
    int a;
    int b;
} dati_bfs;

typedef struct {
    bool *fase_costruzione;
    bool *terminate;
    sigset_t *set;
    attore **attori_ptr;
    int num_attori;
} dati_gestore;

typedef struct {
    char **buffer;
    int *pcindex; // indice del produttore: deve essere 

    attore **attori;
    int num_attori;
    pthread_mutex_t *pmutex;
    sem_t *sem_free_slots;
    sem_t *sem_data_items;
} dati_consumatore;


//------------------------------------------------------------------------------------------------------------------------------
//                                                         FUNZIONI E STRUTTURE DATI UTILI
//------------------------------------------------------------------------------------------------------------------------------

void termina(const char *messaggio);
void attore_distruggi(attore *a);
void attori_distruggi(attore **array_attori, int num_attori);
void* gestore_segnali(void *arg);
void* consumatore(void *arg);
void dati_consumatore_distruggi(dati_consumatore *dc);

void termina(const char *messaggio)

{perror(messaggio);exit(1);}

// Verifica ordinamento
bool is_array_sorted_by_codice(attore **attori, size_t num_attori) {
    for (size_t i = 0; i < num_attori - 1; i++) {
        if (attori[i]->codice > attori[i+1]->codice) return false;
    }
    return true;
}

int shuffle(int n) {
    return ((((n & 0x3F) << 26) | ((n >> 6) & 0x3FFFFFF)) ^ 0x55555555);
}

int unshuffle(int n) {
    return ((((n >> 26) & 0x3F) | ((n & 0x3FFFFFF) << 6)) ^ 0x55555555);
}

attore *attore_crea(int cod, char *nome, int anno) {
    attore *a = malloc(sizeof(*a));
    a->codice = cod;
    a->anno = anno;
    a->nome = strdup(nome);
    a->numcop = 0;
    a->cop = NULL;
    return a;
}

void attore_distruggi(attore *a) {
    if (a == NULL) return;
    free(a->nome);
    free(a->cop);
    free(a);
}

void attore_stampa(attore *a){
    if (a==NULL) printf("non c'è un attore");
    else
    {
        printf("  Codice: %d\n", a->codice);
        printf("  Nome: %s\n", a->nome ? a->nome : "(NULL)");
        printf("  Anno: %d\n", a->anno);
        printf("  Numero di coprotagonisti: %d\n", a->numcop);
        for(int i = 0; i < a->numcop; i++) {
            printf("  Coprotagonista %d: %d\n", i + 1, a->cop[i]);
        }
    }
}

void attori_distruggi(attore **array_attori, int num_attori) {
    if (array_attori == NULL) {
        return;  // Evita segfault se l'array è NULL
    }

    // Dealloca ogni singolo attore nell'array
    for (int i = 0; i < num_attori; i++) {
        if (array_attori[i] != NULL) {
            attore_distruggi(array_attori[i]);  // Usa la funzione di distruzione singola
            array_attori[i] = NULL;  // Opzionale: imposta a NULL dopo la deallocazione
        }
    }

    // Dealloca l'array di puntatori
    free(array_attori);
}

typedef struct nodo_albero_coda {
    int codice;
    attore *a;
    struct nodo_albero_coda *left;
    struct nodo_albero_coda *right;
    struct nodo_albero_coda *succ;
    struct nodo_albero_coda *parent;
    int dist;
} nodo_albero_coda;

typedef struct {
    nodo_albero_coda *testa;
    nodo_albero_coda *coda;
    nodo_albero_coda *radice;
} albero_coda;

/* Funzione di confronto per bsearch */
int cmp_attore_codice(const void *key, const void *elem) {
    int codice = *(const int *)key;            // Il codice che cerchiamo
    const attore *a = *(const attore **)elem;  // Elemento dell'array (attore **)
    return codice - a->codice;                 // Ritorna differenza
}

/* Funzione di ricerca */
attore *trova_attore(int codice, attore **attori, size_t num_attori) {
    if (!attori || num_attori == 0) {
        return NULL;  // Gestione casi edge
    }
   // printf("chiamo bsearch \n");
    // Utilizzo bsearch
    attore **res = (attore **)bsearch(
        &codice,                  // Chiave di ricerca (indirizzo del codice)
        attori,                   // Array ordinato
        num_attori,               // Numero elementi
        sizeof(attore *),         // Dimensione di ogni elemento (puntatore)
        cmp_attore_codice         // Funzione di confronto
    );
    //printf("ho finito bsearch \n");

    return res ? *res : NULL;  // Dereferenzia se trovato, altrimenti NULL
}

nodo_albero_coda *nodo_crea(attore *a) {
    nodo_albero_coda *nodo = malloc(sizeof(*nodo));
    if (!nodo) return NULL;
    nodo->codice = shuffle(a->codice);
    nodo->a = a;
    nodo->left = nodo->right = nodo->succ = nodo->parent = NULL;
    nodo->dist = 0;
    return nodo;
}

albero_coda *crea_albero_coda(nodo_albero_coda *nodo) {
    
    albero_coda *mio_ac = malloc(sizeof(*mio_ac));
    if (nodo == NULL) 
    {
         mio_ac->testa = mio_ac->coda = mio_ac->radice = nodo;
    }

    if (!mio_ac) return NULL;
    mio_ac->testa = mio_ac->coda = mio_ac->radice = nodo;
    return mio_ac;
}

bool abr_cerca(nodo_albero_coda *root, int codice) {
    if (root == NULL) return false;
    if (codice == root->codice) return true;
    if (codice < root->codice)
        return abr_cerca(root->left, codice);
    else
        return abr_cerca(root->right, codice);
}

nodo_albero_coda *abr_inserisci(nodo_albero_coda *rad, nodo_albero_coda *nodo) {
    if (rad == NULL) return nodo;  // Se il nodo radice è NULL, ritorna il nuovo nodo
    if (nodo == NULL) return rad;  // Se il nodo da inserire è NULL, ritorna la radice

    if (nodo->codice < rad->codice)
        rad->left = abr_inserisci(rad->left, nodo);
    else if (nodo->codice > rad->codice)
        rad->right = abr_inserisci(rad->right, nodo);
    return rad;
}

void lista_inserisci(albero_coda *ac, nodo_albero_coda *nodo) {
    nodo->succ = NULL;
    if (ac->coda == NULL) {
        ac->testa = ac->coda = nodo;
    } else {
        ac->coda->succ = nodo;
        ac->coda = nodo;
    }
}

nodo_albero_coda *lista_rimuovi(albero_coda *ac) {
    if (ac->testa == NULL) return NULL;
    nodo_albero_coda *primo = ac->testa;
    ac->testa = primo->succ;
    if (ac->testa == NULL)
        ac->coda = NULL;
    return primo;
}

void distruggi_albero_coda(nodo_albero_coda *root) {
    if (root == NULL) return;
    distruggi_albero_coda(root->left);
    distruggi_albero_coda(root->right);
    free(root);
    root = NULL;  // Imposta a NULL per evitare dangling pointer
}

void ripercorri_e_stampa(nodo_albero_coda *start, nodo_albero_coda *goal, FILE *f) {
    int len = goal->dist;
    nodo_albero_coda **stack = malloc(len * sizeof(nodo_albero_coda*));
    nodo_albero_coda *curr = goal;
    
    for (int i = 0; i < len; i++) {
        stack[i] = curr;
        curr = curr->parent;
    }
    assert(curr->codice == start->codice);  // Assicura che il percorso inizi da 'start'

    for (int i = len - 1; i >= 0; i--) {
        fprintf(f, "%d\t%s\t%d\n",
                stack[i]->a->codice,
                stack[i]->a->nome,
                stack[i]->a->anno);
    }
    free(stack);
}

//------------------------------------------------------------------------------------------------------------------------------
//                                                         THREAD BFS
//------------------------------------------------------------------------------------------------------------------------------

void *tbody_search(void *arg) {
    printf("Thread di ricerca avviato\n");
    dati_bfs *dati = (dati_bfs *)arg;
    int a = dati->a;
    int b = dati->b;
    attore **attori = dati->attori;
    printf("Attori passati al thread : %p\n", attori);
    int num_attori = dati->num_attori;

    attore *attore_a = trova_attore(a, attori, num_attori);
    //printf("a trovato\n");
    attore *attore_b = trova_attore(b, attori, num_attori);
    if (!attore_a) {
        fprintf(stderr, "Attore %d non trovato\n", a);
        return NULL;
    }
    if (!attore_b) {
        fprintf(stderr, "Attore %d non trovato\n", b);
        return NULL;
    }

    char *nomefile;
    if (asprintf(&nomefile, "%d.%d", a, b) == -1) {
        perror("Errore in asprintf");
        return NULL;
    }
    FILE *file = fopen(nomefile, "w");
    free(nomefile);
    if (!file) {
        perror("Errore apertura file");
        return NULL;
    }

    nodo_albero_coda *nodo_a = nodo_crea(attore_a);
    albero_coda *visitati = crea_albero_coda(nodo_a);
    //albero_coda *fuori_dalla_coda = crea_albero_coda(nodo_a);
    bool trovato = false;

    while (visitati->testa != NULL) {
        nodo_albero_coda *curr = lista_rimuovi(visitati);
        
        if (curr->a->codice == b) {
            trovato = true;
            printf("Percorso trovato tra %d e %d\n", a, b);
            ripercorri_e_stampa(nodo_a, curr, file);
            //free(curr);
            break;
        }

        for (int i = 0; i < curr->a->numcop; i++) {
            int codice_figlio = curr->a->cop[i];
            int codice_shuffled = shuffle(codice_figlio);

            if (abr_cerca(visitati->radice, codice_shuffled))
                continue;

            attore *attore_figlio = trova_attore(codice_figlio, attori, num_attori);
            if (!attore_figlio) continue;

            nodo_albero_coda *nodo_figlio = nodo_crea(attore_figlio);
            if (!nodo_figlio) {
                fprintf(stderr, "Errore allocazione nodo figlio\n");
                continue;
            }
            nodo_figlio->parent = curr;
            nodo_figlio->dist = curr->dist + 1;

            abr_inserisci(visitati->radice, nodo_figlio);
            lista_inserisci(visitati, nodo_figlio);
            //printf("Inserito nodo figlio con codice %d\n", nodo_figlio->codice);
        }
        
        //lista_inserisci(fuori_dalla_coda, curr);
        //abr_inserisci(fuori_dalla_coda->radice, curr);
    }

    if (!trovato) {
        fprintf(stderr, "Percorso non trovato tra %d e %d\n", a, b);
    }

    fclose(file);
    distruggi_albero_coda(visitati->radice);
    free(visitati);

    free(dati);
    return NULL;
}


//------------------------------------------------------------------------------------------------------------------------------
//                                                         LEGGI FILE ATTORI E CREA ARRAY
//------------------------------------------------------------------------------------------------------------------------------

attore **leggi_file_attori(FILE *f, int *num_elementi) {
    assert(f != NULL);

    int size = 10;
    int messi = 0;
    *num_elementi=0;
    attore **a = malloc(size * sizeof(*a));

    if (a == NULL)
    {
        printf("Memoria insufficiente riga 352, malloc fallita");
        free(a);
        return NULL;
    }
        

    while (true) {
        int cod, anno;
        char *nome = NULL;  // Usiamo %ms per allocazione dinamica
    
        // Legge una riga alla volta
        int e = fscanf(f, "%d\t%m[^\t]\t%d", &cod, &nome, &anno) ;
        
    
    
        if (e != 3) {
            if (e == EOF) break;
            else {
                    printf("Memoria insufficiente riga 365, fscanf fallita");
                    free(a);
                    return NULL;
                 }
        }
        

        attore *c = attore_crea(cod, nome, anno);
        free(nome);

        if (messi == size) {
            size *= 2;
            a = realloc(a, size * sizeof(*a));
            if (a == NULL)
                {
                    printf("Memoria insufficiente riga 378");
                    free(a);
                    return NULL;
                }
        }

        assert(size > messi);
        a[messi] = c;
        messi += 1;
    }

    *num_elementi = messi;
    return a;
}

//------------------------------------------------------------------------------------------------------------------------------
//                                                         THREAD GESTORE SEGNALI
//------------------------------------------------------------------------------------------------------------------------------

void* gestore_segnali(void *arg) {
    dati_gestore *dati = (dati_gestore *)arg;
    sigset_t *set = dati->set;
    bool *fase_costruzione = dati->fase_costruzione;
    //attore **arrayattori = (dati->attori_ptr);

    int sig;
    int err;

    printf("PID: %d\n", getpid());

    while (1) {
        err = sigwait(set, &sig);
        if (err != 0) {
            perror("sigwait failed");
            free(dati);
            pthread_exit(NULL);
        }

        if (sig == SIGINT) {
            if (*(fase_costruzione)) {
                printf("Costruzione del grafo in corso\n");
            } else {
                printf("Terminazione in corso...\n");
                break;
            }
        } else {
            printf("Segnale %d ricevuto\n", sig);
        }
    }

    //sleep(20); poi va rimesso !!
   *(dati->terminate) = true;
    free(dati);
    pthread_exit(NULL);
}


//------------------------------------------------------------------------------------------------------------------------------
//                                                        THREAD CONSUMATORE
//------------------------------------------------------------------------------------------------------------------------------

void *tbody(void *arg) {

    printf("sono il consumatore\n");
    dati_consumatore *a = (dati_consumatore *)arg;
    attore **attori = a->attori;
    int num_attori = a->num_attori;

    if (!a || !a->buffer || !a->pcindex || !a->pmutex || !a->sem_data_items || !a->sem_free_slots) {
    printf("Errore: puntatore NULL in dati_consumatore\n");
    pthread_exit(NULL);
}


    while (1) {
        sem_wait(a->sem_data_items);
        pthread_mutex_lock(a->pmutex);

        
        int index = *(a->pcindex) % BUF_SIZE;
        //printf("leggo line da %d posizione del buffer\n", index);
        if (a->buffer[index] == NULL) {
            pthread_mutex_unlock(a->pmutex);
            sem_post(a->sem_free_slots);
            printf("Buffer vuoto, nessuna linea da leggere\n");
            break;  // Se il buffer è vuoto, esce dal ciclo
        }
        char *line = strdup(a->buffer[index]);
        *(a->pcindex) += 1;


        pthread_mutex_unlock(a->pmutex);
        sem_post(a->sem_free_slots);

        //printf("linea letta: %s \n\n\n", line);

        
        
        if(!line) break;
        char *saveptr;
        char *token = strtok_r(line, " \t\n", &saveptr);
        if (token == NULL) {
            printf("token codice non letto\n");
            free(line);
            continue;
        }
       //printf("codice letto: %d\n",atoi(token));
        int codice = atoi(token);

        attore *att = trova_attore(codice, attori, num_attori);
        if (att == NULL) {
            fprintf(stderr, "Attore non presente in array: %d\n", codice);
            free(line);
            continue;
        }
       // printf("attore creato:\n %d\n %s \n %d \n",att->codice, att-> nome, att-> anno);

        token = strtok_r(NULL, " \t\n", &saveptr);
        if (token == NULL) {
            fprintf(stderr, "Dati incompleti per attore %d: manca numcop\n", codice);
            free(line);
            continue;
        }
        int numcop = atoi(token);       
        att->numcop = atoi(token);
        

            att->cop = malloc(numcop * sizeof(int));
            if (!att->cop) {
                fprintf(stderr,"malloc array cop fallita");
                free(line);
                exit(EXIT_FAILURE);
            }     

        for (int i = 0; i < numcop; i++) {
            token = strtok_r(NULL, " \t\n", &saveptr);
            if (token == NULL) {
                fprintf(stderr, "Dati incompleti per coprotagonisti attore %d: solo %d coprotagonisti invece di %d\n", codice, i,att->numcop);
                att->numcop = i;
                break;
            }
            
            att->cop[i] = atoi(token);
           //printf("ho messo il copr. %d in attore %d\n",  att->cop[i], att->codice);
        }

        //attore_stampa(att);

        free(line);
    }

    fprintf(stderr, "Consumatore sta per finire\n");
    // vedi se devi deallocare roba
    pthread_exit(NULL);
}



//------------------------------------------------------------------------------------------------------------------------------
//                                                         MAIN
//------------------------------------------------------------------------------------------------------------------------------

int main(int argc, char *argv[]) {

//--------------------- CONTROLLA I PARAMETRI!!!
if (argc != 4) {
        fprintf(stderr, "Uso: %s <file_nomi> <file_grafo> <num_consumatori>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    pthread_t tid;
    sigset_t set;
    bool fase = true;
    bool terminate=false;

//------------------------------------------------------------------------------------------------------------------------------
//                                                         CREO THREAD GESTORE
//------------------------------------------------------------------------------------------------------------------------------

    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    dati_gestore *arg = malloc(sizeof(*arg));
    if (arg == NULL) termina("malloc dati gestore fallita");
    arg->set = &set;
    arg->fase_costruzione = &fase;
    arg->terminate = &terminate;
    


    pthread_create(&tid, NULL, gestore_segnali, arg);

//------------------------------------------------------------------------------------------------------------------------------
//                                                         CREO ATTORI
//------------------------------------------------------------------------------------------------------------------------------
   
int p = atoi(argv[3]); //   numero di consumatori

    printf("sto aprendo i file\n");

     // Apri filenomi in sola lettura
    FILE *f_grafo = fopen(argv[2], "r");
    if (f_grafo == NULL) {
        perror("Errore apertura file");
        return 1;
    }

    // Apri filegrafo in sola lettura
    FILE *f_nomi = fopen(argv[1], "r");
    if (f_nomi == NULL) {
        perror("Errore apertura file");
        return 1;
    }

    printf("ora leggo i file\n");

    int num_attori;
    attore **attori=leggi_file_attori(f_nomi,&num_attori);
     printf("file letti\n");
    if (attori ==NULL) termina("problemi nella lettura del file");

    printf("ecco i primi 5 attori:\n");
    for(int i = 0; i < 5 && i < num_attori; i++) {
        printf("Attore %d: Codice: %d, Nome: %s, Anno: %d\n", i + 1, attori[i]->codice, attori[i]->nome, attori[i]->anno);
    }
    
    int e =fclose(f_nomi);
    if(e!=0) printf ("errore nella chiusura del file  f_nomi\n");
    else printf ("file  f_nomi chiuso correttamente\n");

   

    arg->attori_ptr = attori;
    arg -> num_attori = num_attori;


//------------------------------------------------------------------------------------------------------------------------------
//                                                         INIZIALIZZO ATTORI
//------------------------------------------------------------------------------------------------------------------------------

    int cindex = 0, pindex = 0;
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_t t[p];
    dati_consumatore a[p];
    char *buffer[BUF_SIZE] = {0};

    sem_t sem_free_slots, sem_data_items;
    sem_init(&sem_free_slots, 0, BUF_SIZE);
    sem_init(&sem_data_items, 0, 0);
    
    printf("inizializzo e creo i thread\n");

    for (int i = 0; i < p; i++) {
        a[i].attori = attori;
        a[i].num_attori = num_attori;
        a[i].buffer = buffer;
        a[i].pcindex = &cindex;
        a[i].pmutex = &mu;
        a[i].sem_data_items = &sem_data_items;
        a[i].sem_free_slots = &sem_free_slots;
        pthread_create(&t[i], NULL, tbody, (void*)&a[i]);
    }

   
//------------------------------------------------------------------------------------------------------------------------------
//                                                         LOOP PRODUTTORE
//------------------------------------------------------------------------------------------------------------------------------ 

printf("sto scrivendo sul buffer\n");


    char *line = NULL;
    size_t len = 0;
    //int it=0;
    while (getline(&line, &len, f_grafo) != -1) {
       // it++; //da togliere poi ché è solo per testare
        sem_wait(&sem_free_slots);

        if (buffer[pindex % BUF_SIZE] != NULL) {
            free(buffer[pindex % BUF_SIZE]);
        }

        buffer[pindex % BUF_SIZE] = strdup(line);
        //printf("produttore: ho scritto nella %d posizione del buffer\n", pindex % BUF_SIZE);
        pindex++;
        
        sem_post(&sem_data_items);

    }
    free(line);
    fclose(f_grafo);// metti controllo errore

    printf("invio segnali terminazione\n");


    //--------------------- SCRIVO SEGNALE TERMINAZIONE

    

    for (int i = 0; i < p; i++) {
        sem_wait(&sem_free_slots);
        pthread_mutex_lock(&mu);
        if (buffer[pindex % BUF_SIZE] != NULL) {
            free(buffer[pindex % BUF_SIZE]);
        }
        buffer[pindex % BUF_SIZE] = NULL;
        pindex++;
        pthread_mutex_unlock(&mu);
        sem_post(&sem_data_items);
        // provo ad aggiungere una var condivisa coi thread di terminazione
    }

    // Dealloca le stringhe nel buffer avanzate
    for (int i = 0; i < BUF_SIZE; i++) {
    if (buffer[i] != NULL) {
        free(buffer[i]);
    }
}

    printf("faccio le join\n");
    //----------------------- JOIN THREAD

    for (int i = 0; i < p; i++) {
        pthread_join(t[i], NULL);
    }

    //------------------- DEALLOCO STRUTTURE


    printf("dealloco semafori e mutex\n");

    pthread_mutex_destroy(&mu);
    sem_destroy(&sem_data_items);
    sem_destroy(&sem_free_slots);

    fase = false; // cambia fase: finita costruzione grafo, inizia pipe

    printf("indirizzo dell'array attori creato: %p\n", attori);

    /*// Verifica ordinamento
    bool sorted = is_array_sorted_by_codice(attori, num_attori) ;
    if (sorted) {
        printf("Array di attori ordinato correttamente per codice.\n");
    } else {
        printf("Array di attori NON ordinato per codice.\n");
    }
*/
   /* printf("lunghezza dell'array attori: %d\n", num_attori);
   printf("attori random\n");
   printf("%s  %d\n",attori[12441]->nome, attori[12441]->codice);
   for (int i = 0; i < attori[12441]->numcop; i++) {
        printf(" %d", attori[12441]->cop[i]);
    }
    printf("\n\n\n");
    printf("%s  %d\n",attori[12442]->nome, attori[12442]->codice);
    for (int i = 0; i < attori[12442]->numcop; i++) {
            printf(" %d", attori[12442]->cop[i]);
        }
   printf("\n");

    //stampa_20_attori(attori, num_attori);
    */



//------------------------------------------------------------------------------------------------------------------------------
//                                                         LETTURA DALLA PIPE
//------------------------------------------------------------------------------------------------------------------------------
    //---------------------------------------------- INIZIALIZZO PIPE
   
    printf("inizializzo pipe\n");
    
    if (mkfifo("cammini.pipe", 0666) == -1) {
        perror("mkfifo");
        exit(1);
    }

    int fd = open("cammini.pipe", O_RDONLY);
    if (fd == -1) {
        perror("open cammini.pipe");
        exit(1);
    }

    printf("pipe aperta\n");

    int32_t a_pipe, b_pipe; //  numeri che vado a leggere, a 32 bit
    ssize_t nbytes;
    printf("inizio lettura dalla pipe\n");
    printf("valore di terminate: %d\n", terminate);
    //--------------------------------------------- CICLO LETTURA PIPE
    while (!(terminate)) {
        nbytes = read(fd, &a_pipe, sizeof(int32_t)); // leggo a
        if (nbytes == 0) break;
        if (nbytes != sizeof(int32_t)) {
            perror("read a diversa da 32 bit");
            break;
        }
        printf("ho letto a: %d\n", a_pipe);

        nbytes = read(fd, &b_pipe, sizeof(int32_t));// leggo b
        if (nbytes == 0) break;
        if (nbytes != sizeof(int32_t)) {
            perror("read b diversa da 32 bit");
            break;
        }

        printf("ho letto b: %d\n", b_pipe);

        //-------------------------------------- CREO THREAD BFS

        dati_bfs *arg_bfs = malloc(sizeof(*arg_bfs));
        if (arg_bfs == NULL) termina("malloc dati bfs fallita");
        arg_bfs->attori = attori;
        arg_bfs->a = a_pipe;
        arg_bfs->b = b_pipe;
        arg_bfs->num_attori = num_attori;

        pthread_t tid_bfs;
        if (pthread_create(&tid_bfs, NULL, tbody_search, arg_bfs) != 0) {
            perror("pthread_create");
            free(arg_bfs);
        } else {
            pthread_detach(tid_bfs);
        }
    }

    sleep(20);

    //-----------------------------------------dealloca attori e strutture dati

    close(fd);
    unlink("cammini.pipe");

    

    
    printf("dealloco gli attori\n");
    attori_distruggi(attori, num_attori);
    kill(getpid(), SIGINT);  // Invia SIGINT al processo corrente, verrà preso da gestore segnali

    pthread_join(tid, NULL);
    return 0;
}