#include "gpu_renderer.h"
#include <string.h>
#include <math.h>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

struct SphereGpu {
    float c[3];
    float r;
    float ref;
    float ior;
    float col[3];
    int mat_type;
};

struct CameraGpu {
    float pos[3];
    float target[3];
};

struct SceneGpu {
    float light_pos[3];
    int num_spheres;
    int num_mesh_tris;
    int width;
    int height;
};

struct MeshMat {
    float col[3];
    float ref;
    float ior;
    int mat_type;
};

static const char* shader_src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"constant float EPS=1e-4f; constant int AA=4, MD=4;\n"
"struct S { packed_float3 c; float r; float ref; float ior; packed_float3 col; int mat; };\n"
"struct C { packed_float3 p; packed_float3 t; };\n"
"struct G { packed_float3 lp; int ns; int nt; int w; int h; };\n"
"struct T { packed_float3 v0,v1,v2,n0,n1,n2; int mi; };\n"
"struct M { packed_float3 col; float ref; float ior; int mat; };\n"
"static bool hs(float3 o,float3 d,float3 c,float r,thread float& t) {\n"
"float3 oc=o-c;float a=dot(d,d),b=2*dot(oc,d),cc=dot(oc,oc)-r*r,dlt=b*b-4*a*cc;\n"
"if(dlt<0)return 0;float sd=sqrt(dlt),t1=(-b-sd)/(2*a),t2=(-b+sd)/(2*a);\n"
"t=(t1>EPS)?t1:t2;return t>EPS;}\n"
"static bool ha(float3 o,float3 d,thread float& t,thread float3& n,thread int& i,device const S* s,int c) {\n"
"float b=1e9;bool h=0;i=-1;for(int j=0;j<c;j++){float ti;if(hs(o,d,s[j].c,s[j].r,ti)&&ti<b){b=ti;h=1;i=j;}}\n"
"if(h){t=b;n=normalize(o+d*b-s[i].c);}return h;}\n"
"static bool ht(float3 o,float3 d,float3 v0,float3 v1,float3 v2,thread float& t,thread float& u,thread float& v) {\n"
"float3 e1=v1-v0,e2=v2-v0,pv=cross(d,e2);float det=dot(e1,pv);\n"
"if(fabs(det)<EPS)return 0;float iv=1/det;float3 tv=o-v0;u=dot(tv,pv)*iv;\n"
"if(u<0||u>1)return 0;float3 qv=cross(tv,e1);v=dot(d,qv)*iv;\n"
"if(v<0||u+v>1)return 0;t=dot(e2,qv)*iv;return t>EPS;}\n"
"static float3 tn(float3 v0,float3 v1,float3 v2,float3 n0,float3 n1,float3 n2,float u,float v) {\n"
"float w=1-u-v;float3 n=w*n0+u*n1+v*n2;float l=length(n);return l>EPS?n/l:float3(0,1,0);}\n"
"static bool hf(float3 o,float3 d,thread float& t){if(fabs(d.y)<EPS)return 0;t=-o.y/d.y;return t>EPS;}\n"
"static float3 fc(float3 p){return ((int(p.x)+int(p.z))&1)?float3(.08,.12,.25):float3(.25,.4,.7);}\n"
"static bool is(float3 p,float3 lp,device const S* s,int sc,device const T* t,int tc,int oi) {\n"
"float3 tl=lp-p;float ld=length(tl),rdn=1/ld;float3 rd=tl*rdn,ro=p+rd*EPS;\n"
"for(int i=0;i<sc;i++){if(i==oi)continue;float tt;if(hs(ro,rd,s[i].c,s[i].r,tt)&&tt<ld&&tt>EPS)return 1;}\n"
"for(int i=0;i<tc;i++){float tt,u,v;if(ht(ro,rd,t[i].v0,t[i].v1,t[i].v2,tt,u,v)&&tt<ld&&tt>EPS)return 1;}\n"
"return 0;}\n"
"static float3 tr(float3 o,float3 d,device const S* s,int sc,device const T* t,int tc,device const M* m,int nm,float3 lp,int ii) {\n"
"float3 a=0,th=1;float3 ro=o,rd=d;\n"
"for(int dp=0;dp<=MD;dp++){\n"
"float ts,tf;float3 sn;int si=-1;bool hs0=ha(ro,rd,ts,sn,si,s,sc);\n"
"float tm=1e9;float3 mn=0;int mi=-1;float mu=0,mv=0;\n"
"for(int i=0;i<tc;i++){float ti,u,v;if(ht(ro,rd,t[i].v0,t[i].v1,t[i].v2,ti,u,v)&&ti<tm){tm=ti;mi=i;mu=u;mv=v;}}\n"
"bool hm=mi>=0;bool hf0=hf(ro,rd,tf);\n"
"int ht0=0;float thit;float3 hn;float3 cc=1;float rr=0,ii2=1.5;int mm=0;\n"
"if(hs0&&(!hf0||ts<tf)&&(!hm||ts<tm)){ht0=1;thit=ts;hn=sn;cc=s[si].col;rr=s[si].ref;ii2=s[si].ior;mm=s[si].mat;}\n"
"else if(hm&&(!hf0||tm<tf)){ht0=2;thit=tm;\n"
"int mj=t[mi].mi;if(mj>=0&&mj<nm){cc=m[mj].col;rr=m[mj].ref;ii2=m[mj].ior;mm=m[mj].mat;}\n"
"hn=tn(t[mi].v0,t[mi].v1,t[mi].v2,t[mi].n0,t[mi].n1,t[mi].n2,mu,mv);if(dot(hn,rd)>0)hn=-hn;}\n"
"else if(hf0){ht0=3;thit=tf;}\n"
"if(ht0==0){a+=float3(.1,.1,.2)*th;break;}\n"
"float3 p=ro+rd*thit;\n"
"if(ht0==3){float3 nf3=float3(0,1,0);float3 tl3=lp-p;float3 ld3=normalize(tl3);bool sh3=is(p,lp,s,sc,t,tc,-1);\n"
"float3 fl3=fc(p);float di3=max(0.f,dot(nf3,ld3));float lf3=sh3?.2:1;a+=(fl3*.15+fl3*di3*lf3)*th;break;}\n"
"int plas=(mm==1);float3 tl2=lp-p;float3 ld2=normalize(tl2);\n"
"bool sh2=is(p,lp,s,sc,t,tc,ht0==1?si:-1);\n"
"float di2=max(0.f,dot(hn,ld2));float3 vw2=normalize(ro-p);\n"
"float3 hf2=normalize(ld2+vw2);float sp2=pow(max(0.f,dot(hn,hf2)),plas?32:64);\n"
"float3 amb2=cc*.15;float ss2=plas?.4:.8;float lf2=sh2?0:1;\n"
"float3 base2=amb2+cc*di2*lf2+cc*sp2*ss2*lf2;a+=base2*th;\n"
"if(plas||dp==MD)break;\n"
"float ci=dot(hn,rd);bool en=ci<0;float3 na=en?hn:-hn;ci=en?-ci:ci;\n"
"float n1=en?1:ii2,n2=en?ii2:1;float et=n1/n2;\n"
"float kk=1-et*et*(1-ci*ci);\n"
"float r0=(1-ii2)/(1+ii2);r0=r0*r0;float fr=r0+(1-r0)*pow(1-ci,5);\n"
"if(kk>0){float ct=sqrt(kk);float3 refr=rd*et+na*(et*ci-ct);ro=p+refr*EPS;rd=refr;th*=(1-fr);th*=cc;}\n"
"else{float3 refld=reflect(rd,na);ro=p+refld*EPS;rd=refld;th*=fr*rr;}}\n"
"return a;}\n"
"kernel void rk(device float3* o[[buffer(0)]],constant C& c[[buffer(1)]],constant G& g[[buffer(2)]],device const S* s[[buffer(3)]],device const T* t[[buffer(4)]],device const M* m[[buffer(5)]],uint2 ti[[thread_position_in_grid]],uint2 gr[[threads_per_grid]]){\n"
"int x=ti.x,y=ti.y;if(x>=g.w||y>=g.h)return;\n"
"float3 f=normalize(c.t-c.p),r=normalize(cross(float3(0,1,0),f)),u=cross(f,r);float a3=float(g.w)/float(g.h);\n"
"float3 sm=0;for(int sy=0;sy<AA;sy++)for(int sx=0;sx<AA;sx++){\n"
"float ux=(2*(x+(sx+.5)/AA)/g.w-1)*a3,uy=1-2*(y+(sy+.5)/AA)/g.h;\n"
"sm+=tr(c.p,normalize(f+r*ux+u*uy),s,g.ns,t,g.nt,m,g.nt>0?1:0,g.lp,-1);}\n"
"o[y*g.w+x]=sm/float(AA*AA);}";

Image* render_frame_gpu(const Scene* scene) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return NULL;

        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:shader_src];
        id<MTLLibrary> library = [device newLibraryWithSource:source
                                                       options:nil
                                                         error:&error];
        if (!library) { return NULL; }

        id<MTLFunction> func = [library newFunctionWithName:@"rk"];
        if (!func) { return NULL; }

        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:func error:&error];
        if (!pipeline) { return NULL; }

        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) { return NULL; }

        int w = scene->width, h = scene->height;

        id<MTLBuffer> outBuf = [device newBufferWithLength:w * h * 3 * sizeof(float)
                                                   options:MTLResourceStorageModeShared];

        CameraGpu cd;
        cd.pos[0] = scene->camera_pos.x; cd.pos[1] = scene->camera_pos.y; cd.pos[2] = scene->camera_pos.z;
        cd.target[0] = scene->camera_target.x; cd.target[1] = scene->camera_target.y; cd.target[2] = scene->camera_target.z;
        id<MTLBuffer> camBuf = [device newBufferWithBytes:&cd length:sizeof(CameraGpu)
                                                  options:MTLResourceStorageModeShared];

        int total_mesh_tris = 0;
        for (int i = 0; i < scene->num_meshes; i++)
            total_mesh_tris += scene->meshes[i].num_tris;

        SceneGpu sd;
        sd.light_pos[0] = scene->light_pos.x; sd.light_pos[1] = scene->light_pos.y; sd.light_pos[2] = scene->light_pos.z;
        sd.num_spheres = scene->num_spheres;
        sd.num_mesh_tris = total_mesh_tris;
        sd.width = w; sd.height = h;
        id<MTLBuffer> scBuf = [device newBufferWithBytes:&sd length:sizeof(SceneGpu)
                                                 options:MTLResourceStorageModeShared];

        // Sphere buffer
        size_t ss = scene->num_spheres * sizeof(SphereGpu);
        SphereGpu* sp = (SphereGpu*)malloc(ss);
        for (int i = 0; i < scene->num_spheres; i++) {
            sp[i].c[0] = scene->spheres[i].pos.x; sp[i].c[1] = scene->spheres[i].pos.y; sp[i].c[2] = scene->spheres[i].pos.z;
            sp[i].r = scene->spheres[i].radius;
            sp[i].ref = scene->spheres[i].reflectivity;
            sp[i].ior = scene->spheres[i].ior;
            sp[i].col[0] = scene->spheres[i].color.x; sp[i].col[1] = scene->spheres[i].color.y; sp[i].col[2] = scene->spheres[i].color.z;
            const char* mat = scene->spheres[i].material[0] ? scene->spheres[i].material : "glass";
            sp[i].mat_type = (strcmp(mat, "plastic") == 0) ? 1 : 0;
        }
        id<MTLBuffer> sphereBuf = [device newBufferWithBytes:sp length:ss
                                                     options:MTLResourceStorageModeShared];
        free(sp);

        // Triangle buffer (flatten all meshes)
        id<MTLBuffer> triBuf = nil;
        id<MTLBuffer> matBuf = nil;
        if (total_mesh_tris > 0) {
            TriGpu* tris = (TriGpu*)malloc(total_mesh_tris * sizeof(TriGpu));
            int ti = 0;
            for (int m = 0; m < scene->num_meshes; m++) {
                for (int j = 0; j < scene->meshes[m].num_tris; j++) {
                    memcpy(&tris[ti], &scene->meshes[m].tris[j], sizeof(TriGpu));
                    tris[ti].mesh_idx = m;
                    ti++;
                }
            }
            triBuf = [device newBufferWithBytes:tris length:total_mesh_tris * sizeof(TriGpu)
                                        options:MTLResourceStorageModeShared];
            free(tris);

            // Mesh material buffer
            MeshMat* mats = (MeshMat*)malloc(scene->num_meshes * sizeof(MeshMat));
            for (int i = 0; i < scene->num_meshes; i++) {
                mats[i].col[0] = scene->meshes[i].color.x;
                mats[i].col[1] = scene->meshes[i].color.y;
                mats[i].col[2] = scene->meshes[i].color.z;
                mats[i].ref = scene->meshes[i].reflectivity;
                mats[i].ior = scene->meshes[i].ior;
                const char* mat = scene->meshes[i].material[0] ? scene->meshes[i].material : "glass";
                mats[i].mat_type = (strcmp(mat, "plastic") == 0) ? 1 : 0;
            }
            matBuf = [device newBufferWithBytes:mats length:scene->num_meshes * sizeof(MeshMat)
                                        options:MTLResourceStorageModeShared];
            free(mats);
        }

        id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmdBuf computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:outBuf offset:0 atIndex:0];
        [enc setBuffer:camBuf offset:0 atIndex:1];
        [enc setBuffer:scBuf offset:0 atIndex:2];
        [enc setBuffer:sphereBuf offset:0 atIndex:3];
        [enc setBuffer:triBuf offset:0 atIndex:4];
        [enc setBuffer:matBuf offset:0 atIndex:5];

        MTLSize gridSize = MTLSizeMake(w, h, 1);
        NSUInteger tg_w = MIN(16u, pipeline.threadExecutionWidth);
        NSUInteger tg_h = MIN(16u, pipeline.maxTotalThreadsPerThreadgroup / tg_w);
        MTLSize tgSize = MTLSizeMake(tg_w, tg_h, 1);
        [enc dispatchThreads:gridSize threadsPerThreadgroup:tgSize];
        [enc endEncoding];
        [cmdBuf commit];
        [cmdBuf waitUntilCompleted];

        Image* img = create_image(w, h);
        float* gpu = (float*)outBuf.contents;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int gi = y * w + x;
                size_t ii = (y * w + x) * 3;
                img->data[ii]   = (uint8_t)(fminf(fmaxf(gpu[gi*3],   0.0f), 1.0f) * 255.0f);
                img->data[ii+1] = (uint8_t)(fminf(fmaxf(gpu[gi*3+1], 0.0f), 1.0f) * 255.0f);
                img->data[ii+2] = (uint8_t)(fminf(fmaxf(gpu[gi*3+2], 0.0f), 1.0f) * 255.0f);
            }
        }
        return img;
    }
}
