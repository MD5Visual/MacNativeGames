/* Synthetic GPU data only. Production dispatch matching, resource lifetime, fallback and bilinear scale. */
int mtl_probe_oot_movie(void) {
    mtl_init(); int checked = 0;
    if (!oot_movie_enabled()) MDIE("run the OoT probe with TOTK_OOT_MOVIE_DIRECT=1");
    if (oot_movie_role("49dff010b9832251_b") || oot_movie_role("unknown") ||
        oot_movie_role(oot_movie_convert_key) != 1 || oot_movie_role(oot_movie_final_key) != 2)
        MDIE("movie key matching");
    checked += 4;
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct A { texture2d<float,access::write> out [[id(0)]]; }; kernel void fallback(constant A &a [[buffer(0)]],uint2 p [[thread_position_in_grid]]) {if(p.x<a.out.get_width()&&p.y<a.out.get_height())a.out.write(float4(1,0,1,1),p);}" options:nil error:&error];
    if (!lib) MDIE("movie fixture shader %s",error.localizedDescription.UTF8String);
    NvShader *convert=[NvShader new], *final=[NvShader new];
    convert->rf.nb=1;convert->rf.b[0]=(nvn_rbind){.kind=NVN_RB_IMG,.reg=0,.rw=1,.dtt=MTLTextureType2D};
    final->rf.nb=1;final->rf.b[0]=(nvn_rbind){.kind=NVN_RB_IMG,.reg=1,.aid=0,.rw=1,.dtt=MTLTextureType2D};
    final->fn=[lib newFunctionWithName:@"fallback"];final->ae=[final->fn newArgumentEncoderWithBufferIndex:0];final->arg_len=final->ae.encodedLength;
    final->rf.tg[0]=8;final->rf.tg[1]=8;final->rf.tg[2]=1;final->stage=5;
    snprintf(final->key,sizeof final->key,"%s-probe_b",oot_movie_final_key);
    NvProg *np=[NvProg new];np->sh[5]=final;prog_t program={.serial=0xfffffa00};serial_put(program.serial,np);
    enum { IW=1920,IH=1080,OW=2560,OH=1440 };
    uint8_t *pixels=malloc(IW*IH*4);if(!pixels)MDIE("movie fixture allocation");
    for(int y=0;y<IH;y++)for(int x=0;x<IW;x++) {
        uint8_t *p=pixels+(y*IW+x)*4;p[0]=x*255/(IW-1);p[1]=y*255/(IH-1);p[2]=64;p[3]=153;
    }
    id<MTLBuffer> rb=probe_buf(256*5);op_hdr end={OP_END,8};
    const unsigned xy[5][2]={{0,0},{OW-1,OH-1},{OW/2,OH/2},{17,29},{OW-31,OH-43}};
    for(int mode=0;mode<2;mode++) {
        tex_t t[2]={{0}};pool_t pools[2]={{0}};void *mem[2];size_t size[2];
        for(int j=0;j<2;j++) {
            t[j].serial=0xfffffa10+mode*4+j;
            t[j].d=(nvn_texdesc){.width=j?OW:IW,.height=j?OH:IH,.depth=1,.levels=1,.target=1,.format=j&&mode?0x38:0x25,
                .swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
            nvn_tex_layout(&t[j].d,&t[j].L);size[j]=(t[j].L.size+getpagesize()-1)&~((size_t)getpagesize()-1);
            mem[j]=mmap(NULL,size[j],PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);if(mem[j]==MAP_FAILED)MDIE("movie fixture mmap");
            if(!j&&nvn_swizzle(&t[j].d,0,0,pixels,IW*4,mem[j]))MDIE("movie fixture swizzle");
            pools[j]=(pool_t){.mem=mem[j],.size=size[j],.flags=0x24};t[j].mem=mem[j];t[j].pool=&pools[j];
            mtl_pool_add(mem[j],size[j]);nvn_dirty_add_range(mem[j],size[j]);mtl_reg_tex(j+1,&t[j],NULL);
        }
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);S.img[5][0]=1;S.img[5][1]=2;S.cprog=&program;
        oot_movie_remember(convert);if(!g_oot_movie_decoded)MDIE("movie conversion binding");
        id<MTLTexture> in=g_oot_movie_decoded;
        if(oot_movie_scale(in,in))MDIE("movie accepted wrong destination");checked++;
        convert->rf.b[0].rw=0;if(oot_movie_image(convert,0,NULL,0))MDIE("movie accepted read-only conversion reflection");convert->rf.b[0].rw=1;checked++;
        S.img[5][1]=0;if(oot_movie_replace(final))MDIE("movie accepted missing output");S.img[5][1]=2;checked++;
        /* A stale image cannot survive into another submission. */
        oot_movie_remember(convert);mtl_sub_commit();mtl_finish();mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        if(g_oot_movie_decoded||oot_movie_replace(final))MDIE("movie stale submission accepted");checked++;
        S.cprog=&program;S.img[5][0]=1;S.img[5][1]=2;
        /* No conversion: the production dispatch must execute the magenta fixture kernel. */
        p_dispatch dp={OW/8,OH/8,1,0};dispatch(&dp);
        registry_lock();id<MTLTexture> out=tex_by_id(2);registry_unlock();
        enc_b_copy_texture_buffer(blit(),out,0,0,MTLOriginMake(0,0,0),MTLSizeMake(1,1,1),rb,0,256,256);
        mtl_sub_commit();mtl_finish();const uint8_t *b=rb.contents;
        if(b[0]!=255||b[1]!=0||b[2]!=255||b[3]!=255)MDIE("movie fallback dispatch lost");checked+=4;
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);S.cprog=&program;S.img[5][0]=1;S.img[5][1]=2;
        oot_movie_remember(convert);
        if (!mode) {
            dispatch(&dp);
        } else {
            enum { N = sizeof oot_movie_network / sizeof *oot_movie_network };
            uint8_t control[0x880] = {[0x714] = 5};
            prog_t programs[N - 1] = {0};
            struct { op_hdr bind; p_bind_program program; op_hdr run; p_dispatch dispatch; } ops[N];
            struct { uint8_t bytes[sizeof ops]; op_hdr end; } packet;
            packet.end = end;
            program.nstage = 1;
            program.sd[0].control = control;
            program.sd[0].data = (uintptr_t)control;
            for (unsigned i = 0; i < N; ++i) {
                prog_t *p = &program;
                if (i != N - 1) {
                    p = &programs[i];
                    *p = program;
                    p->serial = 0xfffffb00 + i;
                    NvShader *shader = [NvShader new];
                    snprintf(shader->key, sizeof shader->key, "%s-probe_b", oot_movie_network[i]);
                    NvProg *resolved = [NvProg new];
                    resolved->sh[5] = shader;
                    serial_put(p->serial, resolved);
                }
                ops[i].bind = (op_hdr){OP_BIND_PROGRAM, sizeof(op_hdr) + sizeof(p_bind_program)};
                ops[i].program = (p_bind_program){p, NVN_SHADER_STAGE_COMPUTE_BIT};
                ops[i].run = (op_hdr){OP_DISPATCH, sizeof(op_hdr) + sizeof(p_dispatch)};
                ops[i].dispatch = dp;
            }
            ops[1].bind.op = OP_FENCE_SYNC;
            memcpy(packet.bytes, ops, sizeof ops);
            if (oot_movie_try_network(packet.bytes)) MDIE("movie accepted a side-effect command");
            ops[1].bind.op = OP_BIND_PROGRAM;
            memcpy(packet.bytes, ops, sizeof ops);
            S.img[5][1] = 0;
            if (oot_movie_try_network(packet.bytes)) MDIE("movie accepted a missing sequence output");
            S.img[5][1] = 2;
            g_oot_movie_skip_end = oot_movie_try_network(packet.bytes);
            if (g_oot_movie_skip_end != packet.bytes + (N - 1) * sizeof ops[0] + sizeof(op_hdr) + sizeof(p_bind_program))
                MDIE("movie sequence was not fully recognized");
            mtl_exec(packet.bytes);
            if (g_oot_movie_skip_end) MDIE("movie sequence skip state leaked");
            checked += 4;
            for (unsigned i = 0; i < N - 1; ++i) mtl_prog_release(&programs[i]);
        }
        if(g_oot_movie_decoded||!gpu_owned(t[1].serial)||S.cprog!=&program||S.img[5][1]!=2)MDIE("movie state/ownership");checked+=4;
        registry_lock();out=tex_by_id(2);registry_unlock();
        for(int i=0;i<5;i++)enc_b_copy_texture_buffer(blit(),out,0,0,MTLOriginMake(xy[i][0],xy[i][1],0),MTLSizeMake(1,1,1),rb,i*256,256,256);
        mtl_sub_commit();mtl_finish();
        for(int i=0;i<5;i++) {
            float sx=fmaxf(0,fminf(IW-1,(xy[i][0]+.5f)*IW/OW-.5f)),sy=fmaxf(0,fminf(IH-1,(xy[i][1]+.5f)*IH/OH-.5f));
            unsigned x0=(unsigned)sx,y0=(unsigned)sy,x1=MIN(x0+1,IW-1),y1=MIN(y0+1,IH-1);float tx=sx-x0,ty=sy-y0;
            for(int c=0;c<4;c++) {
                float v=((1-ty)*((1-tx)*pixels[(y0*IW+x0)*4+c]+tx*pixels[(y0*IW+x1)*4+c])+ty*((1-tx)*pixels[(y1*IW+x0)*4+c]+tx*pixels[(y1*IW+x1)*4+c]))/255.f;
                if(mode&&c<3)v=v<=.0031308f?v*12.92f:1.055f*powf(v,1/2.4f)-.055f;
                int got=((uint8_t *)rb.contents)[i*256+c],want=lroundf(v*255);
                if(abs(got-want)>2)MDIE("movie scale mode%d sample%d channel%d got%d want%d",mode,i,c,got,want);checked++;
            }
        }
        S.cprog=NULL;S.img[5][0]=S.img[5][1]=0;
        for(int j=0;j<2;j++){mtl_tex_release(&t[j]);mtl_pool_del(mem[j]);nvn_dirty_remove_range(mem[j]);mtl_finish();munmap(mem[j],size[j]);}
    }
    mtl_prog_release(&program);res_remove(rb);free(pixels);return checked;
}
