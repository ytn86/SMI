/**
  Reduce for FLOAT
  This will use two "tags":

*/

#ifndef REDUCE_H
#define REDUCE_H
#pragma OPENCL EXTENSION cl_khr_fp64 : enable

#include "smi/data_types.h"
#include "smi/header_message.h"
#include "smi/network_message.h"
#include "smi/operation_type.h"

//temp here, then need to move

channel SMI_Network_message channel_reduce_send __attribute__((depth(1)));
channel SMI_Network_message channel_reduce_send_no_root __attribute__((depth(1))); //TODO: tmp, we need to distinguish in the support kernel
channel SMI_Network_message channel_reduce_recv __attribute__((depth(1)));



//align to 64 to remove aliasing
typedef struct __attribute__((packed)) __attribute__((aligned(64))){
    SMI_Network_message net;          //buffered network message
    char tag_out;                    //Output channel for the bcast, used by the root
    char tag_in;                     //Input channel for the bcast. These two must be properly code generated. Good luck
    char root_rank;
    char my_rank;                   //These two are essentially the Communicator
    char num_rank;
    uint message_size;              //given in number of data elements
    uint processed_elements;        //how many data elements we have sent/received
    char packet_element_id;         //given a packet, the id of the element that we are currently processing (from 0 to the data elements per packet)
    SMI_Datatype data_type;               //type of message
    char size_of_type;              //size of data type
    char elements_per_packet;       //number of data elements per packet
    SMI_Network_message net_2;        //buffered network message
    char packet_element_id_rcv;     //used by the receivers
}SMI_RChannel;


SMI_RChannel SMI_Open_reduce_channel(uint count, SMI_Datatype data_type, char root, char my_rank, char num_ranks)
{
    SMI_RChannel chan;
    //setup channel descriptor
    chan.message_size=count;
    chan.data_type=data_type;
    chan.tag_in=0;
    chan.tag_out=0;
    chan.my_rank=my_rank;
    chan.root_rank=root;
    chan.num_rank=num_ranks;
    chan.size_of_type=4;
    chan.elements_per_packet=1;

    //setup header for the message
    SET_HEADER_DST(chan.net.header,root);
    SET_HEADER_SRC(chan.net.header,my_rank);
    SET_HEADER_TAG(chan.net.header,0);        //used by destination
    SET_HEADER_NUM_ELEMS(chan.net.header,0);    //at the beginning no data
    chan.processed_elements=0;
    chan.packet_element_id=0;
    chan.packet_element_id_rcv=0;
    return chan;
}



void SMI_Reduce(SMI_RChannel *chan, volatile void* data_snd, volatile void* data_rcv)
{

    char *conv=(char*)data_snd;
    #pragma unroll
    for(int jj=0;jj<4;jj++) //copy the data
        chan->net.data[jj]=conv[jj];

    //In this case we disabled network packetization: so we can just send the data as soon as we have it
    SET_HEADER_NUM_ELEMS(chan->net.header,1);
        //offload to reduce kernel
    if(chan->my_rank==chan->root_rank) //root
        write_channel_intel(channel_reduce_send,chan->net);
    else //non root
        write_channel_intel(channel_reduce_send_no_root,chan->net);
    chan->packet_element_id=0;

    if(chan->my_rank==chan->root_rank)//I'm the root, I have to receive from the kernel
    {
        chan->net_2=read_channel_intel(channel_reduce_recv);

        char * ptr=chan->net_2.data;
        *(float *)data_rcv= *(float*)(ptr);

#if 0
        if(chan->packet_element_id_rcv==0)
            chan->net_2=read_channel_intel(channel_reduce_recv);

        char * ptr=chan->net_2.data+(chan->packet_element_id_rcv)*4;
        chan->packet_element_id_rcv++;                       //first increment and then use it: otherwise compiler detects Fmax problems
        if(chan->packet_element_id_rcv==7)
            chan->packet_element_id_rcv=0;
        *(float *)data_rcv= *(float*)(ptr);
#endif
    }


}


__kernel void kernel_reduce(const char num_rank)
{
    //printf("Reduce kernel\n");
    SMI_Network_message mess,mess_no_root;
    SMI_Network_message reduce;
    bool init=true;
    char sender_id=0;
    const char credits_flow_control=7; //apparently, this combination (credits, max ranks) is the max that we can support with II=1
    const char max_num_ranks=16;
    float reduce_result[credits_flow_control];  //reduced results
    char data_recvd[credits_flow_control];
    bool send_credits=false;//true if (the root) has to send reduce request
    char credits=credits_flow_control; //the number of credits that I have
    char send_to=0;
    char __attribute__((register)) add_to[max_num_ranks];   //for each rank tells to what element in the buffer we should add the received item
    for(int i=0;i<credits_flow_control;i++)
    {
        data_recvd[i]=0;
        reduce_result[i]=0;
    }

    for(int i=0;i<max_num_ranks;i++)
        add_to[i]=0;
    char current_buffer_element=0;
    char add_to_root=0;
    const int READS_LIMIT=8;
    int contiguos_reads=0; //this is used only for reading from CK_R
    while(true)
    {
        bool valid=false;
        if(!send_credits)
        {
            switch(sender_id)
            {   //for the root, I have to receive from both sides
                case 0:
                    mess=read_channel_nb_intel(channel_reduce_send,&valid);
                    break;
                case 1: //read from CK_R, can be done by the root and by the non-root
                    mess=read_channel_nb_intel(channels_from_ck_r[/*chan_idx*/0],&valid);
                    break;
            }
            if(valid)
            {
                char a;
                if(sender_id==0)
                {
                    //simply send this to the attached CK_S
                    //we have to distinguish whether this is the root or not
                    //if(GET_HEADER_DST(mess.header)==GET_HEADER_SRC(mess.header)) //root
                    // {
                    //ONLY the root can receive from here
                    char rank=GET_HEADER_DST(mess.header);
                    //char addto=add_to[rank];
                    char * ptr=mess.data;
                    float data= *(float*)(ptr);
                    reduce_result[add_to_root]+=data;        //SMI_ADD
                    // printf("Reduce kernel received from app, root. Adding it to: %d \n",add_to_root);
                    data_recvd[add_to_root]++;
                    a=add_to_root;
                    send_credits=init;      //the first reduce, we send this
                    init=false;
                    add_to_root++;
                    if(add_to_root==credits_flow_control)
                        add_to_root=0;
                    //add_to[rank]=addto;

                }
                else
                {
                    contiguos_reads++;
                    //received from CK_R,
                    //apply reduce
                    //   printf("Reduce kernel, received from remote\n");

                    if(GET_HEADER_OP(mess.header)==SMI_REQUEST)//i'm not the root
                    {
                        //this is a credit! I can take the message from the app and send it
                        mess_no_root=read_channel_intel(channel_reduce_send_no_root);
                        write_channel_intel(channels_to_ck_s[0],mess_no_root);
                    }
                    else
                    {
                        //I'm the root
                        char * ptr=mess.data;
                        char rank=GET_HEADER_SRC(mess.header);
                        float data= *(float*)(ptr);
                        //printf("Reduce kernel, received from %d: %d\n",rank,data);
                        char addto=add_to[rank];
                        data_recvd[addto]++;
                        a=addto;
                        reduce_result[addto]+=data;        //SMI_ADD
                        addto++;
                        //add_to[rank]++;
                        if(addto==credits_flow_control)
                            addto=0;
                        add_to[rank]=addto;

                    }

                }
                //printf("a: %d, data_recvd: %d\n",(int)a,(int)data_recvd[a]);
                //data_recvd[a]++;
                //printf("reduce result %d, received %d out of %d\n",(int)a,(int)data_recvd[a],num_rank);
                if(data_recvd[current_buffer_element]==num_rank)
                {   //This could happens only in the root

                    //printf("Reduce kernel, send to app: %d\n",reduce_result[current_buffer_element]);
                    //send to application
                    char *data_snd=reduce.data;
                    char *conv=(char*)(&reduce_result[current_buffer_element]);
                    #pragma unroll
                    for(int jj=0;jj<4;jj++) //copy the data
                        data_snd[jj]=conv[jj];
                    write_channel_intel(channel_reduce_recv,reduce);
                    send_credits=true;
                    credits++;
                    data_recvd[current_buffer_element]=0;
                    reduce_result[current_buffer_element]=0;
                    current_buffer_element++;
                    if(current_buffer_element==credits_flow_control)
                        current_buffer_element=0;

                }
            }
            if(sender_id==0)
                sender_id=1;
            else
                if(!valid || contiguos_reads==READS_LIMIT)
                {
                    sender_id=0;
                    contiguos_reads=0;
                }
        }
        else
        {
            //root: send credits
            if(send_to!=GET_HEADER_DST(mess.header))
            {
                SET_HEADER_OP(reduce.header,SMI_REQUEST);
                SET_HEADER_TAG(reduce.header,0); //TODO: Fix this tag
                SET_HEADER_DST(reduce.header,send_to);
                write_channel_intel(channels_to_ck_s[1],reduce);

            }
            send_to++;
            if(send_to==num_rank)
            {
                send_to=0;
                credits--;
                send_credits=(credits!=0);
            }

        }
        //mem_fence(CLK_CHANNEL_MEM_FENCE);

    }

}


#endif // REDUCE_H