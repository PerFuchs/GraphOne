#pragma once

#include "sstream_view.h"

#ifdef _MPI

#ifdef B32    
    MPI_Datatype mpi_type_vid = MPI_UINT32_T;
#elif B64 
    MPI_Datatype mpi_type_vid = MPI_UINT32_T;
#endif

#define BUF_TX_SZ (1<<20)

template <class T>
struct part_t {
    index_t changed_v;
    index_t changed_e;
    int position;
    int back_position;
    int delta_count;
    int buf_size;
    int rank;
    vid_t dst_offset;
    char* buf;

    void init() {
        //header_size + (changed_v<<1)*sizeof(vid_t) + changed_e*sizeof(T);
        buf_size = BUF_TX_SZ; 
        buf = (char*) malloc(sizeof(char)*buf_size);
        
        position = 0;
        changed_v = 0;
        changed_e = 0;
        back_position = 0;
        delta_count = 0;
        rank = 0;
        dst_offset = 0;
    }
    void reset() {
        changed_v = 0;
        changed_e = 0;
        position = 5*sizeof(uint64_t);
        delta_count = 0;
    }    
}; 

template <class T>
class scopy2d_server_t : public sstream_t<T> {
 public:
    using sstream_t<T>::pgraph;
    using sstream_t<T>::sstream_func;
    int   part_count;
    part_t<T>* _part;
 protected:
    using sstream_t<T>::degree_in;
    using sstream_t<T>::degree_out;
    using sstream_t<T>::graph_out;
    using sstream_t<T>::graph_in;
    using sstream_t<T>::snapshot;
    using sstream_t<T>::edges;
    using sstream_t<T>::edge_count;
    using sstream_t<T>::new_edges;
    using sstream_t<T>::new_edge_count;
    using sstream_t<T>::v_count;
    using sstream_t<T>::flag;
    using sstream_t<T>::bitmap_in;
    using sstream_t<T>::bitmap_out;

 public:
    inline scopy2d_server_t():sstream_t<T>() {
        part_count = 0;
    }
    inline ~scopy2d_server_t() {}

    void        init_view(pgraph_t<T>* pgraph, index_t a_flag);
    status_t    update_view();
 
 private:   
    void  init_buf();
    void  prep_buf(degree_t* degree, onegraph_t<T>* graph);
    void  buf_vertex(part_t<T>* part, vid_t vid);
    void  send_buf(part_t<T>* part);
    void  send_buf_one(part_t<T>* part, int j, int partial = 1);
};

template <class T>
class scopy2d_client_t : public sstream_t<T> {
 public:
    using sstream_t<T>::pgraph;
    using sstream_t<T>::snapshot;
    using sstream_t<T>::sstream_func; 
    using sstream_t<T>::thread;
    using sstream_t<T>::v_count;
    using sstream_t<T>::flag;
    using sstream_t<T>::graph_in;
    using sstream_t<T>::graph_out;
    using sstream_t<T>::degree_in;
    using sstream_t<T>::degree_out;
    using sstream_t<T>::bitmap_in;
    using sstream_t<T>::bitmap_out;

 public:
    vid_t    global_vcount;
    vid_t    v_offset;
    vid_t    dst_offset;
    int      buf_size;
    char*    buf;

 public:
    inline    scopy2d_client_t():sstream_t<T>() {dst_offset = 0;}
    inline    ~scopy2d_client_t() {}
   
    void      init_view(pgraph_t<T>* ugraph, index_t a_flag); 
    status_t  update_view();
 private:
    index_t  apply_view(degree_t* degree, onegraph_t<T>* graph, Bitmap* bitmap);
};


template <class T>
void scopy2d_server_t<T>::init_buf()
{
    part_t<T>* part = (part_t<T>*)malloc(sizeof(part_t<T>)*part_count*part_count);
    _part = part;
    int tid = omp_get_thread_num();
    vid_t v_local = v_count/part_count;
    int header_size = 5*sizeof(uint64_t);

    for (int j = 0; j < part_count*part_count; ++j) {
        part[j].init();
        part[j].position = header_size;
        part[j].rank = 1 + j;
        part[j].dst_offset = (j%part_count)*v_local;
    }
}

template <class T>
void scopy2d_server_t<T>::send_buf_one(part_t<T>* part, int j, int partial /*= 1*/)
{
    int header_size = 5*sizeof(uint64_t);
    uint64_t  archive_marker = snapshot->marker;
    //directions, prop_id, tid, snap_id, vertex size, edge size(dst vertex+properties)
    uint64_t meta_flag = 1;
    if (partial) {
        meta_flag = partial;
    }
    int t_pos = 0;
    
    cout << " sending to rank " << part[j].rank //<< " = "<< v_start << ":"<< v_end 
         << " size "<< part[j].position 
         << " changed_v " << part[j].changed_v
         << " changed_e " << part[j].changed_e
         << endl;
     
    pack_meta(part[j].buf, part[j].buf_size, t_pos, meta_flag, archive_marker, 
              part[j].changed_v, part[j].changed_e);
    assert(t_pos == header_size);
    MPI_Send(part[j].buf, part[j].position, MPI_PACKED, part[j].rank, 0, MPI_COMM_WORLD);
    part[j].reset();
}

template <class T>
void scopy2d_server_t<T>::send_buf(part_t<T>* _part1)
{
    int tid = omp_get_thread_num();
    part_t<T>* part = _part + tid*part_count;
    for (int j = 0; j < part_count; ++j) {
        send_buf_one(part, j);
    }
}

template <class T>
void scopy2d_server_t<T>::buf_vertex(part_t<T>* part, vid_t vid)
{
    part->changed_v++;
    part->changed_e += part->delta_count;
    MPI_Pack(&vid, 1, mpi_type_vid, part->buf, part->buf_size, &part->back_position, MPI_COMM_WORLD);
    MPI_Pack(&part->delta_count, 1, mpi_type_vid, part->buf, part->buf_size, &part->back_position, MPI_COMM_WORLD);
    part->delta_count = 0;
}

/*
template <class T>
void scopy2d_server_t<T>::prep_buf(degree_t* degree, onegraph_t<T>* graph)
{
    int tid = omp_get_thread_num();
    part_t<T>* part = _part + tid*part_count;
    
    vid_t v_local = v_count/part_count;
    vid_t v_start = tid*v_local;
    vid_t v_end =  v_start + v_local;
    //if (tid == part_count - 1) v_end = v_count;
    
    //Lets copy the data
    snapid_t     snap_id = snapshot->snap_id;
    degree_t  nebr_count = 0;
    degree_t   old_count = 0;
    header_t<T> delta_adjlist;
    sid_t sid;
    vid_t vid;
    T dst;
    int j = 0;
    
    for (vid_t v = v_start; v < v_end; ++v) { 
        nebr_count = graph->get_degree(v, snap_id);
        old_count = degree[v];
        if (old_count == nebr_count) continue;

        vid = v - v_start;
        nebr_count -= old_count; 
        graph->start(v, delta_adjlist, old_count);
        for (degree_t i = 0; i < nebr_count; ++i) {
            graph->next(delta_adjlist, dst);
            sid = get_sid(dst);
            for (j = 0;  j < part_count - 1; ++j) {
                 if(sid < part[j+1].dst_offset) break;
            }
            part[j].delta_count += 1;
        }

        for (int j = 0; j < part_count; ++j) {
            if (0 == part[j].delta_count) {
                continue;
            }
            //XXX Check if we can process next vertex
            if (total_size + this_size > part[j].buf_size) {
            } else {
                part[j].back_position = part[j].position;
                buf_vertex(part + j, vid);
            }
        }
    }
    
    for(vid_t v = v_start; v < v_end; ++v) {
        nebr_count = graph->get_degree(v, snap_id);
        old_count = degree[v];
        if (old_count == nebr_count) continue;
        degree_out[v] = nebr_count;
        
        vid = v - v_start;
        nebr_count -= old_count; 
        graph->start(v, delta_adjlist, old_count);
        for (degree_t i = 0; i < nebr_count; ++i) {
            graph->next(delta_adjlist, dst);
            sid = get_sid(dst);
            for (j = 0;  j < part_count - 1; ++j) {
                 if(sid < part[j+1].dst_offset) break;
            }
            set_sid(dst, sid - part[j].dst_offset);
            MPI_Pack(&dst, 1, pgraph->data_type, part[j].buf, part[j].buf_size, &part[j].position, MPI_COMM_WORLD);
            part[j].delta_count += 1;
        }
        
    }
}
*/
template <class T>
void scopy2d_server_t<T>::prep_buf(degree_t* degree, onegraph_t<T>* graph)
{
    int tid = omp_get_thread_num();
    part_t<T>* part = _part + tid*part_count;
    
    vid_t v_local = v_count/part_count;
    vid_t v_start = tid*v_local;
    vid_t v_end =  v_start + v_local;
    //if (tid == part_count - 1) v_end = v_count;
    
    //Lets copy the data
    snapid_t     snap_id = snapshot->snap_id;
    degree_t  nebr_count = 0;
    degree_t   old_count = 0;
    header_t<T> delta_adjlist;
    sid_t sid;
    vid_t vid;
    T dst;
    int j = 0;
    
    for (vid_t v = v_start; v < v_end; ++v) { 
        nebr_count = graph->get_degree(v, snap_id);
        old_count = degree[v];
        if (old_count == nebr_count) continue;

        vid = v - v_start;
        degree[v] = nebr_count;
        for (int j = 0; j < part_count; j++) {
            if (part[j].position + 2*sizeof(vid_t) + sizeof(T) >= part[j].buf_size) {
                send_buf_one(part, j, 2);
            }
            part[j].back_position = part[j].position;
            part[j].position += 2*sizeof(vid_t);
        }
        
        nebr_count -= old_count; 
        graph->start(v, delta_adjlist, old_count);
        for (degree_t i = 0; i < nebr_count; ++i) {
            graph->next(delta_adjlist, dst);
            sid = get_sid(dst);
            for (j = 0;  j < part_count - 1; ++j) {
                 if(sid < part[j+1].dst_offset) break;
            }
            //send and reset the data
            if (part[j].position  + sizeof(T)>= part[j].buf_size) {
                buf_vertex(part + j, vid);
                send_buf_one(part, j, 2);
                
                part[j].back_position = part[j].position;
                part[j].position += 2*sizeof(vid_t);
            }
            
            set_sid(dst, sid - part[j].dst_offset);
            MPI_Pack(&dst, 1, pgraph->data_type, part[j].buf, part[j].buf_size, &part[j].position, MPI_COMM_WORLD);
            part[j].delta_count += 1;
        }

        for (int j = 0; j < part_count; ++j) {
            if (0 == part[j].delta_count) {
                part[j].position -= 2*sizeof(vid_t);
                continue;
            }
           
            buf_vertex(part + j, vid);
        }
    }
}

template <class T>
void scopy2d_server_t<T>::init_view(pgraph_t<T>* pgraph, index_t a_flag) 
{
    sstream_t<T>::init_view(pgraph, a_flag);
    part_count = _part_count;
    init_buf();
}

template <class T>
status_t scopy2d_server_t<T>::update_view()
{
    blog_t<T>* blog = pgraph->blog;
    index_t  marker = blog->blog_head;
    
    snapshot_t* new_snapshot = pgraph->get_snapshot();
    
    if (new_snapshot == 0|| (new_snapshot == snapshot)) return eNoWork;
    index_t old_marker = snapshot? snapshot->marker: 0;
    index_t new_marker = new_snapshot->marker;
    
    snapshot = new_snapshot;
    
#pragma omp parallel num_threads(part_count)
{
    if (graph_in == graph_out) {
        prep_buf(degree_out, graph_out);
        send_buf(_part);    
    } else {
        prep_buf(degree_out, graph_out);
        send_buf(_part);    
        prep_buf(degree_in, graph_in);
        send_buf(_part);    
    }
}

return eOK;
}

template <class T>
status_t scopy2d_client_t<T>::update_view()
{
    index_t archive_marker;
    if (graph_in == graph_out) {
        archive_marker = apply_view(degree_out, graph_out, bitmap_out);
    } else {
        archive_marker = apply_view(degree_out, graph_out, bitmap_out);
        archive_marker = apply_view(degree_in, graph_in, bitmap_in);
    }
    
    pgraph->new_snapshot(archive_marker);
    snapshot = pgraph->get_snapshot();
    return eOK;
}

template <class T>
index_t scopy2d_client_t<T>::apply_view(degree_t* degree, onegraph_t<T>* graph, Bitmap* bitmap)
{
    bitmap->reset();

    //Lets copy the data
    MPI_Status status;
    int  partial = 0;
    uint64_t  archive_marker = 0;
    uint64_t flag = 0;//directions, prop_id, tid, snap_id, vertex size, edge size (dst vertex +  properties)
    int position = 0;
    index_t changed_v = 0;
    index_t changed_e = 0;
    //int buf_size = 0;
    //char*    buf = 0;

    do {
        MPI_Probe(0, 0, MPI_COMM_WORLD, &status);
        MPI_Get_count(&status, MPI_CHAR, &buf_size);
        //cout << " Rank " << _rank << " MPI_get count = " << buf_size << endl;

        MPI_Recv(buf, buf_size, MPI_PACKED, 0, 0, MPI_COMM_WORLD, &status);
        position = 0;

        unpack_meta(buf, buf_size, position, flag, archive_marker, changed_v, changed_e);
        partial = flag;

        cout << "Rank " << _rank << ":" << v_offset
             << " Archive Marker = " << archive_marker 
             << " size "<< buf_size 
             << " changed_v " << changed_v
             << " changed_e " << changed_e
             << endl;

        vid_t            vid = 0;
        degree_t delta_count = 0;
        degree_t      icount = 0;
        degree_t    prior_sz = 16384;
        T* local_adjlist = (T*)malloc(prior_sz*sizeof(T));

        for (vid_t v = 0; v < changed_v; ++v) {
            MPI_Unpack(buf, buf_size, &position, &vid, 1, mpi_type_vid, MPI_COMM_WORLD);
            MPI_Unpack(buf, buf_size, &position, &delta_count, 1, mpi_type_vid, MPI_COMM_WORLD);
            
            graph->increment_count_noatomic(vid, delta_count);
            //Update the degree of the view
            degree[vid] += delta_count;
            bitmap->set_bit(vid);
            
            while (delta_count > 0) {
                icount = delta_count > prior_sz ? prior_sz : delta_count;
                MPI_Unpack(buf, buf_size, &position, local_adjlist, icount, pgraph->data_type, MPI_COMM_WORLD);
                graph->add_nebr_bulk(vid, local_adjlist, icount);
                delta_count -= icount;
            }
        }
    } while(2 == partial);

    //cout << "Rank " << _rank << " done" << endl;
    return archive_marker;
}

template <class T>
void scopy2d_client_t<T>::init_view(pgraph_t<T>* ugraph, index_t a_flag)
{
    snap_t<T>::init_view(ugraph, a_flag);
    global_vcount = _global_vcount;
    
    bitmap_out = new Bitmap(v_count);
    if (graph_out == graph_in) {
        bitmap_in = bitmap_out;
    } else {
        bitmap_in = new Bitmap(v_count);
    }
    
    int row_id = (_rank - 1)/_part_count;
    int col_id = (_rank - 1)%_part_count;
    v_offset = row_id*(global_vcount/_part_count);
    dst_offset = col_id*(global_vcount/_part_count);
    buf_size = BUF_TX_SZ; 
    buf = (char*) malloc(sizeof(char)*buf_size);
}
#endif
