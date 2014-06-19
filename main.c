#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
//#include <conio.h>
#include <sys/types.h>
#include <sys/stat.h>
/* -------- aux stuff ---------- */
void* mem_alloc(size_t item_size, size_t n_item)
{
	size_t *x = calloc(1, sizeof(size_t)*2 + n_item * item_size);
	x[0] = item_size;
	x[1] = n_item;
	return x + 2;
}
void* mem_extend(void *m, size_t new_n)
{
	size_t *x = (size_t*)m - 2;
	x = realloc(x, sizeof(size_t) * 2 + *x * new_n);
	if (new_n > x[1])
		memset((char*)(x + 2) + x[0] * x[1], 0, x[0] * (new_n - x[1]));	
	x[1] = new_n;
	return x + 2;
}
inline void _clear(void *m)
{
	size_t *x = (size_t*)m - 2;
	memset(m, 0, x[0] * x[1]);
}

#define _new(type, n)	mem_alloc(sizeof(type), n)
#define _del(m)		{ free((size_t*)(m) - 2); m = 0; }
#define _len(m)		*((size_t*)m - 1)
#define _setsize(m, n)	m = mem_extend(m, n)
#define _extend(m)	m = mem_extend(m, _len(m) * 2) 
/* ----------- LZW stuff -------------- */
typedef uint8_t byte;
typedef uint16_t ushort;
#define M_CLR	256	/* clear table marker */
#define M_EOD	257	/* end-of-data marker */
#define M_NEW	258	/* new code index */ 
/* encode and decode dictionary structures.   
	for encoding, entry at code index is a list of indices that follow current one,
   i.e. if code 97 is 'a', code 387 is 'ab', and code 1022 is 'abc',
   then dict[97].next['b'] = 387, dict[387].next['c'] = 1022, etc. */ 

typedef struct 
{	
	ushort next[256];
} lzw_enc_t;

/* for decoding, dictionary contains index of whatever prefix index plus trailing
   byte.  i.e. like previous example,
   dict[1022] = { c: 'c', prev: 387 },
   dict[387]  = { c: 'b', prev: 97 },
   dict[97]   = { c: 'a', prev: 0 } 
   the "back" element is used for temporarily chaining indices when resolving 
   a code to bytes */

typedef struct 
{
	ushort prev, back;
	byte c;
} lzw_dec_t;

byte* lzw_encode(byte *in, int max_bits)
{
	int len = _len(in), bits = 9, next_shift = 512;
	ushort code, c, nc, next_code = M_NEW;
	lzw_enc_t *d =_new(lzw_enc_t, 512);
 	if (max_bits > 16) max_bits = 16;
	if (max_bits < 9 ) max_bits = 12;
 	byte *out = _new(ushort, 4);
	int out_len = 0, o_bits = 0;
	uint32_t tmp = 0;
 	inline void write_bits(ushort x)
	{
		tmp = (tmp << bits) | x;
		o_bits += bits;
		if (_len(out) <= out_len) _extend(out);
		while (o_bits >= 8)
		{
			o_bits -= 8;
			out[out_len++] = tmp >> o_bits;
			tmp &= (1 << o_bits) - 1;
		}
	}
	//write_bits(M_CLR);
	for (code = *(in++); --len; )
	{
		c = *(in++);
		if ((nc = d[code].next[c]))
		code = nc;
		else 
		{
			write_bits(code);
			nc = d[code].next[c] = next_code++;
			code = c;
		}
		/* next new code would be too long for current table */
		if (next_code == next_shift) 
		{
			/* either reset table back to 9 bits */
			if (++bits > max_bits) 
			{
				/* table clear marker must occur before bit reset */
				write_bits(M_CLR);
 				bits = 9;
				next_shift = 512;
				next_code = M_NEW;
				_clear(d);
				} 
			else			/* or extend table */
			_setsize(d, next_shift *= 2);
		}
	}
 	write_bits(code);
	write_bits(M_EOD);
	if (tmp) write_bits(tmp);
 	_del(d);
 	_setsize(out, out_len);
	return out;
}

byte* lzw_decode(byte *in)
{
	byte *out =_new(byte, 4);
	int out_len = 0;
 	inline void write_out(byte c)
	{
		while (out_len >= _len(out)) _extend(out);
		out[out_len++] = c;	
	}
 	lzw_dec_t *d =_new(lzw_dec_t, 512);
	int len, j, next_shift = 512, bits = 9, n_bits = 0;
	ushort code, c, t, next_code = M_NEW;
 	uint32_t tmp = 0;
	inline void get_code()
	{
		while(n_bits < bits) 
		{
			if (len > 0)
			{
				len --;
				tmp = (tmp << 8) | *(in++);
				n_bits += 8;
				} 
			else
			{
				tmp = tmp << (bits - n_bits);
				n_bits = bits;
			}
		}
		n_bits -= bits;
		code = tmp >> n_bits;
		tmp &= (1 << n_bits) - 1;
	}
 	inline void clear_table()
	{
		_clear(d);
		for (j = 0; j < 256; j++) d[j].c = j;
		next_code = M_NEW;
		next_shift = 512;
		bits = 9;
	};
 	clear_table();
	/* in case encoded bits didn't start with M_CLR */
	for (len = _len(in); len;)
	{	
		get_code();
		if (code == M_EOD) break;
		if (code == M_CLR)
		{
			clear_table();
			continue;                        
		} 
		if (code >= next_code)
		{
			fprintf(stderr, "Bad sequence\n");
			_del(out);
			goto bail;
		}
 		d[next_code].prev = c = code;
		while (c > 255)
		{
			t = d[c].prev; 
            d[t].back = c; 
            c = t;
		}
 		d[next_code - 1].c = c;
 		while (d[c].back)
		{
			write_out(d[c].c);
			t = d[c].back;
             d[c].back = 0; 
             c = t;
		}
		write_out(d[c].c);
 		if (++next_code >= next_shift)
		{
			if (++bits > 16)
			{
			/* if input was correct, we'd have hit M_CLR before this */
				fprintf(stderr, "Too many bits\n");
				_del(out);
				goto bail;
			}
			_setsize(d, next_shift *= 2);
		}
	} 
	/* might be ok, so just whine, don't be drastic */
	if (code != M_EOD)
		fputs("Bits did not end in EOD\n", stderr);
	_setsize(out, out_len);
    bail:
	_del(d);
	return out;
}




/*************************************************************

        Huffman Encoding
        
**************************************************************/
struct node
{
	char c;
	int freq;
	struct node *link;
	struct node *rlink;
	struct node *llink;
};

typedef struct node node_t;

struct List
{
	node_t *head;
};

typedef struct List List_t;

struct code_generated
{
    unsigned char c :8;
};

struct info
{
	int freq;
	unsigned char ch;
};

typedef struct info info;

typedef struct code_generated code_generated;
void encode(char *,char *);

void insert_in_list(List_t *,node_t *);

void init_list(List_t *ptr);

void disp_list(const List_t *ptr);

void make_node(List_t *ptr,int n,char c);

void make_list(List_t *ptr,int *freq);

void find_freq(FILE *f1,int *freq);

void make_tree(List_t *ptr,int freq[]);

void find_code(node_t *ptr,char code[],char codes[256][40]);

void compress(FILE *fp,FILE *en,char codes[256][40],int no_of_chars,info header[],int freq[],int num);

unsigned char convert_string_char(char string[9]);

int find_no_of_chars(int freq[]);

void strre(char *,int);

node_t * decode_char(FILE *de,node_t *,int,int,node_t *,int *,int);

void decode(char *,char *);

int create_header(int *,int,info []);





void encode(char *file_name1,char *file_name2)
{
	FILE *fp,*en;
	struct stat st1;
   	int freq[256]={0};
	info header[256];	
	char codes[256][40];
	char code[40];
	strcpy(code,"");
	List_t mylist_en;
	int num,no_of_chars;
	en=fopen(file_name2,"w");
	fp=fopen(file_name1,"r");
	if(fp==0)
	{
             printf("\nFile does not exit");
            
             exit(0);
    }
	fstat(fileno(fp), &st1);
    printf("\nThe size of original file is %ld\t",st1.st_size);
	if(fp)
	{	
		find_freq(fp,freq);
		fclose(fp);		
		no_of_chars=find_no_of_chars(freq);
		fp=fopen(file_name1,"r");
		if(no_of_chars)
		{
		make_tree(&mylist_en,freq);	
		num=create_header(freq,no_of_chars,header);		
		find_code(mylist_en.head,code,codes);
		compress(fp,en,codes,no_of_chars,header,freq,num);
        fclose(en);
        en=fopen(file_name2,"r");		
		struct stat st2;
    	fstat(fileno(en), &st2);
        printf("\nThe size of encoded file is %ld\t",st2.st_size);
        }
		else
		{
			printf("\nFile Is Empty\n");
		}
	}
	else 
	{
		printf("\nFile Doesnot Exist\n");
	}
	fclose(fp);
	fclose(en);
}

void decode(char *file_name1,char *file_name2)//to decode a file
{
	info y,*ptrrr;  
    	FILE *en,*de;
    	ptrrr=&y;
    	code_generated x,*ptrr;
    	ptrr=&x;
    	List_t mylist_de;    
    	int no_of_chars;
   	int freq[256]={0};
    	int i,num; 
    	int count=0;
    	en=fopen(file_name1,"r");
    	de=fopen(file_name2,"w");
	if(en)
	{
    		fread(ptrr,sizeof(code_generated),1,en);
    		num=ptrr->c;       
    		for(i=0;i<num;++i)
    		{
          		fread(ptrrr,sizeof(info),1,en);
           		freq[(int)ptrrr->ch]=ptrrr->freq;
    		}       
    		make_tree(&mylist_de,freq);            
    		no_of_chars=find_no_of_chars(freq);    		    
    		node_t *root=mylist_de.head;
    		node_t *ptr=root;
    		while(fread(ptrr,sizeof(code_generated),1,en)==1)
    		{   
       			ptr=decode_char(de,ptr,ptrr->c,1,root,&count,no_of_chars);
    		}
	}
	else
	{
		printf("\nFile Doesnot Exist\n");
	}
	fclose(en);
	fclose(de);
}

//to decode each charcter of compressed file
node_t * decode_char(FILE *de,node_t *ptr,int x,int y,node_t *root,int *count,int no_of_chars)
{
    	if(y!=9 && (*count)!=no_of_chars)//y to track no.of bits used,count to track no of charcters decoded
    	{
        	if(ptr->llink==0 && ptr->rlink==0)//leaf node
        	{
            		fputc(ptr->c,de);
            		ptr=root;
	    		++(*count);
            		ptr=decode_char(de,ptr,x,y,root,count,no_of_chars);
        	}
        	else if(x%2==0 && ptr->llink)
        	{
            		ptr=decode_char(de,ptr->llink,x/2,++y,root,count,no_of_chars);
        	}
        	else if(x%2!=0 && ptr->rlink)
        	{
            		ptr=decode_char(de,ptr->rlink,x/2,++y,root,count,no_of_chars);
        	}
    	}
    	return ptr;
}

void insert_in_list(List_t *,node_t *);//to insert node into priority queue


void init_list(List_t *ptr)//initializing list of nodes
{
	ptr->head=0;
}


void disp_list(const List_t *ptr)//to display the list
{
	node_t *temp=ptr->head;
	while(temp)
	{	
		printf("%d( %c )\t",temp->freq,temp->c);
		temp=temp->link;
	}
	printf("\n");
}


void make_node(List_t *ptr,int n,char c)//to make node of type node_t
{
	node_t *temp;
	temp=(node_t *)malloc(sizeof(node_t));
	temp->freq=n;
	temp->c=c;
	temp->link=0;
	temp->rlink=0;
	temp->llink=0;
	insert_in_list(ptr,temp);
}


void insert_in_list(List_t *ptr,node_t *t)//node inserted in listin ascending order according to value of frequency
{
	if(ptr->head==0)//empty list
	{
		ptr->head=t;
	}
	else
	{
		node_t *prev=0;
		node_t *pres=ptr->head;
		while(pres && pres->freq<=t->freq)
		{
			prev=pres;
			pres=pres->link;
		}
		if(pres==ptr->head)
		{
			ptr->head=t;
			t->link=pres;
		}
		else if(pres==0)
		{
			prev->link=t;

		}
		else
		{
			t->link=pres;
			prev->link=t;
		}
	}
}


void make_list(List_t *ptr,int *freq)//to create list of nodes with frequency
{
	int i;
	for(i=0;i<256;++i)
	{
		if(freq[i]!=0)
		{
			make_node(ptr,freq[i],i);
		}
	}
}

void  find_freq(FILE *fp,int *freq)//to find frequency of each character
{
	char c;
	while((c=fgetc(fp))!=EOF)
	{
		++freq[(int)c];//subscript denotes the ascii value of character		
	}
}
void make_tree(List_t *ptr,int freq[])//to create tree
{
	node_t *t1,*t2;//pointer to first two nodes of queue    		
	init_list(ptr);
        make_list(ptr,freq);       		
	while(ptr->head->link!=0)
	{
		t1=ptr->head;
		t2=ptr->head->link;
		ptr->head=t2->link;
		t1->link=0;
		t2->link=0;
		node_t *temp;
		temp=(node_t *)malloc(sizeof(node_t));
		temp->freq=t1->freq+t2->freq;
		temp->c=0;
		temp->link=0;
		temp->rlink=t2;
		temp->llink=t1;
		insert_in_list(ptr,temp);//new node is added in ascending order
	}	
}

void find_code(node_t *ptr,char code[],char codes[256][40])//to find code for each character in tree
{    
    	char temp[40];//temporary storage of intermediate code    
    	if(ptr->rlink==0 && ptr->llink==0)//base condition
    	{
        	strcpy(codes[(int)(ptr->c)],code);
    	}
    	if(ptr->rlink!=0)//pointer points right
    	{
       		strcpy(temp,code);
        	strcat(temp,"1");//1 added at the end of intermediate code
        	find_code(ptr->rlink,temp,codes);
    	}
    	if(ptr->llink!=0)//pointer points left
    	{
        	strcpy(temp,code);
        	strcat(temp,"0");//0 added at the end of intermediate code
        	find_code(ptr->llink,temp,codes);
    	}   
}

int find_no_of_chars(int freq[])//to find no. of chars in original file
{
    	int i,sum=0;
    	for(i=0;i<256;++i)//0-255 ascii characters only
    	{
       		sum=sum+freq[i];
    	}
    	return sum;
}

void compress(FILE *fp,FILE *en,char codes[256][40],int no_of_chars,info header[],int freq[],int num)//to store a character of original file into encoded file
{
    	char ch;
    	int i=0,j=0,count=0,k,l;//i to keep track of string of code,j to keep track of code of each character,count to keep track of number of each character encoded
    	unsigned char c;//stores binary eqiuivalent of code    
    	code_generated x;
    	char string_rev[9];
    	x.c=num;    
    	fwrite(&x,sizeof(code_generated),1,en);	
    	for(k=0,l=0;k<256;++k)
    	{
		if(freq[k])
		{
			fwrite(&header[l],sizeof(info),1,en);			
			++l;
		}	
    	}	
    	while((ch=fgetc(fp))!=EOF)
    	{
       		j=0;
        	++count;
        	while(codes[(int)ch][j]!='\0')
        	{
            		string_rev[i]=codes[(int)ch][j];
            		++i;
            		++j;
            		if(i==8)
            		{
                		string_rev[i]='\0';
                		strre(string_rev,strlen(string_rev));//string of code is reversed since binary numbers are read from right to left              
                		c=convert_string_char(string_rev);
                		strcpy(string_rev,"");
                		i=0;
                		x.c=c;                
                		fwrite(&x,sizeof(x),1,en);
             		}
            		if(count==no_of_chars )//for last character
            		{
                		while(codes[(int)ch][j])
                		{
                        		string_rev[i]=codes[(int)ch][j];
                        		++i;
                        		++j;
                        		if(i==8)//if code of last character occupies all bits
                        		{
                            			string_rev[i]='\0';
                            			strre(string_rev,strlen(string_rev));                           
                            			c=convert_string_char(string_rev);
                            			strcpy(string_rev,"");
                            			i=0;
                            			x.c=c;
                            			fwrite(&x,sizeof(x),1,en);
                        		}
                		}
                	string_rev[i]='\0';
                	strre(string_rev,strlen(string_rev));                
                	c=convert_string_char(string_rev);
                	strcpy(string_rev,"");
                	x.c=c;
                	fwrite(&x,sizeof(x),1,en);
            		}
        	}
    	}
    	
}




void strre(char *str,int n)//to reverse a string,n is length of string
{
    	int i=0,j=n-1;
    	char temp;
    	while(i<j)
    	{
       		temp=str[i];
        	str[i]=str[j];
        	str[j]=temp;
        	++i;
        	--j;
    	}
   	str[n]='\0';
}
int create_header(int *freq,int no_of_chars,info header[])
{	
	int i,j;
	for(i=0,j=0;i<256;++i)
	{
		if(freq[i]!=0)
		{
			header[j].freq=freq[i];
			header[j].ch=i;			
			++j;
		}
	}
	return j;	
}
unsigned char convert_string_char(char string[8])//to convert string of code into character

{
    	int i=0;
    	unsigned char c=0;//initially c=00000000
	while(string[i])
	{
        	if(string[i]=='1')//to add 1 at the end of binary of c
		{
			c=c*2;//equivalent to bitwise left shift by 1 unit
			c=c+1;
		}
        	else//to add 0 at the end of binary of c
        	{
            		c=c*2;
        	}
        	++i;
    	}
    	return c;
}

/****************************************

            Main function()
            
****************************************/
int main()
{
    int choice1;
    char choice2;
    do
    {
    printf("Choice the compression algorithm\n");
    printf("1.LZW Algo \n2.Huffmann Encode Algo\n3.Rle Algo\n4.Shannon Fano\t");
    scanf("%d",&choice1);
    switch(choice1)
    {
    case 1:                                                    //LZW option
        { char file[20],encoded_file[20],decoded_file[20];
         printf("Enter the file name\t");
         scanf("%s",file);
         printf("\n");
        // printf("Enter the encoded file name\t");
        // scanf("%s",encoded_file);
        // printf("\n");
        // printf("Enter the decoded file name\t");
        // scanf("%s",decoded_file);
        //printf("\n");
	     int i, fd = open(file, O_RDONLY);                     //Original file
	    // int encoded=open(encoded_file,O_CREAT|O_WRONLY);  //encoded file
	    // int decoded=open(decoded_file,O_CREAT|O_WRONLY);  //decoded file
 	     if (fd == -1)
	     {
		        fprintf(stderr, "Can't read file\n");
		        		        return 1;
          };
 	      struct stat st;
 	      fstat(fd, &st);
 	      byte *in =_new(char, st.st_size);
	      read(fd, in, st.st_size);
	      _setsize(in, st.st_size);
	      close(fd);
 	      printf("input size:   %d\n", _len(in));
 	
 	      byte *enc = lzw_encode(in, 9);
	      printf("encoded size: %d\n", _len(enc));
	      //write(encoded,enc,st.st_size);
	      //close(encoded);
	
 	      byte *dec = lzw_decode(enc);
	     // printf("decoded size: %d\n", _len(dec));
	      //write(decoded,dec,st.st_size);
 	      //close(decoded);
	
 	      for (i = 0; i < _len(dec); i++)
		      if (dec[i] != in[i])
		      {
			   printf("bad decode at %d\n", i);
			   break;
               }
         // if (i == _len(dec))// printf("Decoded ok\n");
          _del(in);
	      _del(enc);
	      _del(dec);
	      
 	      break;
        } 
      case 2:                                                       //Huffmann Encode option
         {  int choice;
	       char file_name1[50],file_name2[50];    	
	       int flag=1;
	       while(flag)
	       {
		              printf("\n\n\t\t\tMENU\n1.Compress File\n0.Exit\n\nEnter Choice\n");
		              scanf("%d",&choice);
		              switch(choice)
		              {
                       case 1:			
                            printf("\nEnter Name Of File To Be Encoded\n");
				            scanf("%s",file_name1);			
				            printf("\nEnter Name Of Encoded File\n");
				            scanf("%s",file_name2);	
				            encode(file_name1,file_name2);
				            break;
                       case 2:				
				            printf("\nEnter Name Of Encoded File\n");
                            scanf("%s",file_name1);    					
				            printf("\nEnter Name Of Decompressed File \n");
                            scanf("%s",file_name2);				
    				        decode(file_name1,file_name2);				
				            break;
                      case 0:
				           flag=0;
				           break;
                      default:
	                       printf("\nWrong Choice Entered...Enter Again\n");
                    }
               }
              
                break;
      }
      
      
      case 3:                                                      //rle algo
      {
           char filenm[500],enco[500];
           int count=0,setcount=1,temp=255;
           
           printf("\nEnter the file name:\n");
           scanf("%s",filenm);
           printf("\nEnter the encoded name:\n");
           scanf("%s",enco);
           
           int fi=open(filenm,O_RDONLY);
           FILE* fenc=fopen(enco,"w");
           if(fi==-1)
           {
                     fprintf(stderr, "Can't read file\n");
                     return 1;
           };
           struct stat st4;
           fstat(fi, &st4);
           byte *a =_new(char, st4.st_size);
           read(fi, a, st4.st_size);
           //byte *b=_new(char, st4.st_size);
	      
          int len=st4.st_size;
          printf("\n\nLength:%d",len);

          while(count<len)
          {
	                      if(a[count]==a[count+1])
	                      {
	                                              setcount++;
	                                              count++;
                          }

	                      else
		                      while(1)
		                      {
		                              if(setcount>255)
			                          {
              	                                      //printf("%c\\%d",a[count],temp);
                                                      setcount-=temp;
                                      }
                                      else
			                          {
          	      		                              //printf("%c\\%d",a[count],setcount);
                                                      fprintf(fenc, " %c %d", a[count],setcount);
			                                          setcount=1;
			                                          count++;
			                                          break;
                                      }

                               }

           }
           //write(fenc,a,st4.st_size);
           //enc=open(fenc,O_RDONLY);
           fclose(fenc);
           FILE* fenc2=fopen(enco,"r");
           struct stat st5;
           fstat(fileno(fenc2),&st5);
           int len2=st5.st_size;
           printf("\nEncoded size is : %d",len2);
	       printf("\n");
           close(fi);
           fclose(fenc2);
	       return(0);
        }
        
      default:
              printf("\nWrong Choice\n");
      }
      printf("\nWant to continue(y/n)\n");
     // fflush(stdin);
      scanf("%c",&choice2);
      printf("\n\n\n");
  }while(choice2=='y' || choice2=='Y');
      
  }
  

  
  
              
