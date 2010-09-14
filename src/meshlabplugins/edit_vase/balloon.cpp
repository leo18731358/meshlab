#include "balloon.h"
#include "float.h"
#include "math.h"
#include "vcg/complex/trimesh/update/curvature.h"
#include "vcg/complex/trimesh/update/curvature_fitting.h"

using namespace vcg;

//---------------------------------------------------------------------------------------//
//
//                                   LOGIC
//
//---------------------------------------------------------------------------------------//
void Balloon::init( int gridsize, int gridpad ){
    //--- Reset the iteration counter
    numiterscompleted = 0;

    //--- Instantiate a properly sized wrapping volume
    vol.init( gridsize, gridpad, cloud.bbox );
    qDebug() << "Created a volume of sizes: " << vol.size(0) << " " << vol.size(1) << " " << vol.size(2);

    //--- Compute hashing of ray intersections (using similar space structure of volume)
    gridAccell.init( vol, cloud );
    qDebug() << "Finished hashing rays into the volume";

    //--- Construct EDF of initial wrapping volume (BBOX)
    // Instead of constructing isosurface exactly on the bounding box, stay a bit large,
    // so that ray-isosurface intersections will not fail for that region.
    // Remember that rays will take a step JUST in their direction, so if they lie exactly
    // on the bbox, they would go outside. The code below corrects this from happening.
    Box3f enlargedbb = cloud.bbox;
    // float del = .99*vol.getDelta(); // almost +1 voxel in each direction
    float del = 1.99*vol.getDelta(); // USED FOR DEBUG
    
    // float del = .50*vol.getDelta(); // ADEBUG: almost to debug correspondences
    Point3f offset( del,del,del );
    enlargedbb.Offset( offset );
    vol.initField( enlargedbb );  // init volumetric field with the bounding box
    
    //--- Extract initial zero level set surface
    vol.isosurface( surf, 0 ); // qDebug() << "Extracted balloon isosurface (" << surf.vn << " " << surf.fn << ")";
    //--- Clear band for next isosurface, clearing the corresponding computation field
    for(unsigned int i=0; i<vol.band.size(); i++){
        Point3i& voxi = vol.band[i];
        MyVoxel& v = vol.Voxel(voxi);
        v.status = 0;
        v.face = 0;
        v.index = 0;
        v.field = NAN;
    }
    vol.band.clear();
    //--- Update correspondences & band
    vol.band.reserve(5*surf.fn);
    vol.updateSurfaceCorrespondence( surf, gridAccell, 2*vol.getDelta() );
}

bool Balloon::initializeField(){
    //--- Setup the interpolation system
    // if we fail, signal the failure to the caller
    // Lower levels of omega might cause overshoothing problems
    // float OMEGA = 1e3; // 1e8
    float OMEGA = 1e8; // 1e8

    LAPLACIAN type = COTANGENT; // COMBINATORIAL
    bool op_succeed = finterp.Init( &surf, 1, type );
    if( !op_succeed ){
        finterp.ColorizeIllConditioned( type );
        return false;
    }


    float OMEGA_VERTEX = 1e-1;
    float OMEGA_DATA   = 1e-1;
    winterp.Init( &surf, 1, type );
    if( !op_succeed ){
        winterp.ColorizeIllConditioned( type );
        return false;
    }

    
    // Shared data
    enum INITMODE {BIFACEINTERSECTIONS, FACEINTERSECTIONS, BOXINTERSECTIONS} mode;
    // mode = BIFACEINTERSECTIONS;
    mode = BIFACEINTERSECTIONS;
    const float ONETHIRD = 1.0f/3.0f;
    float t,u,v; // Ray-Triangle intersection parameters
    Point3f fcenter;
    Point3i off;

    // Uses the face centroid as a hash key in the accellGrid to determine which
    // rays might intersect the current face.
    if( mode == BOXINTERSECTIONS ){
        for(CMeshO::FaceIterator fi=surf.face.begin();fi!=surf.face.end();fi++){
            CFaceO& f = *(fi);
            fcenter = f.P(0) + f.P(1) + f.P(2);
            fcenter = myscale( fcenter, ONETHIRD );
            gridAccell.pos2off( fcenter, off );
            //--- examine intersections and determine real ones...
            PointerVector& rays = gridAccell.Val(off[0], off[1], off[2]);
            f.C() = ( rays.size()>0 ) ? Color4b(255,0,0,255) : Color4b(255,255,255,255);
        }
        qDebug() << "WARNING: test-mode only, per vertex field not updated!!";
    }  
    // Each intersecting ray gives a contribution on the face to each of the
    // face vertices according to the barycentric weights
    else if( mode == FACEINTERSECTIONS ){
        this->rm ^= SURF_VCOLOR;
        this->rm |= SURF_FCOLOR;
        surf.face.EnableColor();
        surf.face.EnableQuality();
        tri::UpdateQuality<CMeshO>::FaceConstant(surf, 0);
        std::vector<float> tot_w( surf.fn, 0 );
        for(CMeshO::FaceIterator fi=surf.face.begin();fi!=surf.face.end();fi++){
            CFaceO& f = *(fi);
            f.ClearS();
            f.C() = Color4b(255,255,255, 255);
            f.Q() = 0; // initialize
            Point3f fcenter = f.P(0) + f.P(1) + f.P(2);
            fcenter = myscale( fcenter, ONETHIRD );
            // NEW/OLD ITERATOR
            // gridAccell.pos2off( fcenter, off );
            // PointerVector& prays = gridAccell.Val(off[0], off[1], off[2]);
            // for(unsigned int i=0; i<prays.size(); i++){ Ray3f& ray = prays[i]->ray;
            for(gridAccell.iter.first(fcenter); !gridAccell.iter.isDone(); gridAccell.iter.next()){
                Ray3f& ray = gridAccell.iter.currentItem().ray;
                if( vcg::IntersectionRayTriangle<float>(ray, f.P(0), f.P(1), f.P(2), t, u, v) ){
                    // Color the faces, if more than one, take average
                    tot_w[ tri::Index(surf,f) ]++;
                    f.Q() += t; // normalize with tot_w after
                    f.SetS();
                    //--- Add the constraints to the field
                    finterp.AddConstraint( tri::Index(surf,f.V(0)), OMEGA*(1-u-v), t );
                    finterp.AddConstraint( tri::Index(surf,f.V(1)), OMEGA*(u), t );
                    finterp.AddConstraint( tri::Index(surf,f.V(2)), OMEGA*(v), t );
                }
            }
        }

        //--- Normalize in case there is more than 1 ray per-face
        for(CMeshO::FaceIterator fi=surf.face.begin();fi!=surf.face.end();fi++){
            if( tot_w[ tri::Index(surf,*fi) ] > 0 )
                fi->Q() /= tot_w[ tri::Index(surf,*fi) ];
        }
        //--- Transfer the average distance stored in face quality to a color
        //    and do it only for the selection (true)
        tri::UpdateColor<CMeshO>::FaceQualityRamp(surf, true);
    }
    // This is the variation that adds to add constraints on both sides of the isosurface
    // to cope with noise but especially to guarantee convergence of the surface. If we go
    // through a sample, a negative force field will be generated that will push the
    // isosurface back close to the sample.
    else if( mode == BIFACEINTERSECTIONS ){
        this->rm ^= SURF_VCOLOR;
        this->rm |= SURF_FCOLOR;
        surf.face.EnableColor();
        surf.face.EnableQuality();
        tri::UpdateQuality<CMeshO>::FaceConstant(surf, 0);
        std::vector<float> tot_w( surf.fn, 0 );

        // We clear the pokingRay-triangle correspondence information + distance information
        // to get ready for the next step
        gridAccell.clearCorrespondences();

        // In this first phase, we scan through faces and we update the information
        // contained in the rays. We try to have a many-1 correspondence in between
        // rays and faces: each face can have more than one ray, but one ray can only
        // have one face associated with it. This face can either be behind or in
        // front of the ray startpoint.
        for(CMeshO::FaceIterator fi=surf.face.begin();fi!=surf.face.end();fi++){
            CFaceO& f = *(fi);
            f.ClearS();
            f.C() = Color4b(255,255,255, 255);
            f.Q() = 0; // initialize
            Point3f fcenter = f.P(0) + f.P(1) + f.P(2);
            fcenter = myscale( fcenter, ONETHIRD );
            gridAccell.pos2off( fcenter, off );
            PointerVector& prays = gridAccell.Val(off[0], off[1], off[2]);

            // We check each of the possibly intersecting rays and we associate this face
            // with him if and only if this face is the closest to it. Note that we study
            // the values of t,u,v directly as a t<0 is accepted as intersecting.
            for(gridAccell.iter.first(f); !gridAccell.iter.isDone(); gridAccell.iter.next()){
            // for(gridAccell.iter.first(fcenter); !gridAccell.iter.isDone(); gridAccell.iter.next()){
                PokingRay& pray = gridAccell.iter.currentItem();
                Line3<float> line(pray.ray.Origin(), pray.ray.Direction());

            // for(unsigned int i=0; i<prays.size(); i++){
            //    Line3<float> line(prays[i]->ray.Origin(), prays[i]->ray.Direction());
                
                // If the ray falls within the domain of the face
                if( IntersectionLineTriangle(line, f.P(0), f.P(1), f.P(2), t, u, v) ){
                
                    // DEBUG
                    // f.C() = t>0 ? Color4b(0,0,255,255) : f.C() = Color4b(255,0,0,255);
                    
                    // DEBUG
                    // if( prays[i]->f!=NULL && fabs(t)<fabs(prays[i]->t) ){
                    //    prays[i]->f->C() = Color4b(255,0,0,255);
                    //    qDebug() << "replaced: " << tri::Index(surf,prays[i]->f) << " from t: " << prays[i]->t << " to " << tri::Index(surf,f) << t;
                    // }
                    
                    // If no face was associated with this ray or this face is closer
                    // than the one that I stored previously
                    if( pray.f==NULL || fabs(t)<fabs(pray.t) ){
                        pray.f=&f;
                        pray.t=t;
                    }
                }
            }
        }
       
        //--- Add constraints on vertexes of balloon
        for(CMeshO::VertexIterator vi=surf.vert.begin();vi!=surf.vert.end();vi++)
            winterp.AddConstraint( tri::Index(surf,*vi), OMEGA_VERTEX, 0 );

        // Now we scan through the rays, we visit the "best" corresponding face and we
        // set a constraint on this face. Also we modify the color of the face so that
        // an approximation can be visualized
        for(unsigned int i=0; i<gridAccell.rays.size(); i++){
            // Retrieve the corresponding face and signed distance
            Ray3f& ray = gridAccell.rays[i].ray;
            // The use of vcg::Simplify to remove degenerate triangles could cause a ray to fail the intersection test!!
            if( gridAccell.rays[i].f == NULL){
                qDebug() << "warning: ray #" << i << "has provided NULL intersection"; // << toString(ray.Origin()) << " " << toString(ray.Direction());
                continue;
            }
            CFaceO& f = *(gridAccell.rays[i].f);
            float t = gridAccell.rays[i].t;
            assert( !math::IsNAN(t) );

            // Color the faces, if more than one, take average
            tot_w[ tri::Index(surf,f) ]++;
            f.Q() += t; // normalize with tot_w after
            f.SetS(); // enable the face for coloring
 
            // DEBUG
            // f.C() = Color4b(0,255,0,255);
 
            // I was lazy and didn't store the u,v... we need to recompute them
            vcg::IntersectionRayTriangle<float>(ray, f.P(0), f.P(1), f.P(2), t, u, v);

            //--- Add the barycenter-weighted constraints to the vertices of the face
            finterp.AddConstraint( tri::Index(surf,f.V(0)), OMEGA*(1-u-v), t );
            finterp.AddConstraint( tri::Index(surf,f.V(1)), OMEGA*(u), t );
            finterp.AddConstraint( tri::Index(surf,f.V(2)), OMEGA*(v), t );

            //--- And for the second interpolator
            winterp.AddConstraint( tri::Index(surf,f.V(0)), OMEGA_DATA*(1-u-v), 1 );
            winterp.AddConstraint( tri::Index(surf,f.V(1)), OMEGA_DATA*(u),     1 );
            winterp.AddConstraint( tri::Index(surf,f.V(2)), OMEGA_DATA*(v),     1 );

            assert( u>=0 && u<=1 && v>=0 && v<=1 );
        }
        
        //--- Normalize in case there is more than 1 ray per-face
        for(CMeshO::FaceIterator fi=surf.face.begin();fi!=surf.face.end();fi++){
            if( tot_w[ tri::Index(surf,*fi) ] > 0 )
                fi->Q() /= tot_w[ tri::Index(surf,*fi) ];
        }
        //--- Transfer the average distance stored in face quality to a color and do it only for the selection (true)
        tri::UpdateColor<CMeshO>::FaceQualityRamp(surf, true);
    }
    
    return true;
}
void Balloon::interpolateField(){
    //--- Mark property active
    surf.vert.QualityEnabled = true;

    //--- Interpolate the field
    // finterp.SolveInQuality();

    //--- Interpolate the field
    winterp.SolveInQuality();

    //--- Transfer vertex quality to surface
    rm &= ~SURF_FCOLOR; // disable face colors
    rm |=  SURF_VCOLOR; // enable vertex colors
    Histogram<float> H;
    tri::Stat<CMeshO>::ComputePerVertexQualityHistogram(surf,H);
    tri::UpdateColor<CMeshO>::VertexQualityRamp(surf,H.Percentile(0.0f),H.Percentile(1.0f));
}
void Balloon::computeCurvature(){
    #if FALSE
        float OMEGA = 1; // interpolation coefficient
        sinterp.Init( &surf, 3, COMBINATORIAL );  
        for( CMeshO::VertexIterator vi=surf.vert.begin();vi!=surf.vert.end();vi++ )
            sinterp.AddConstraint3( tri::Index(surf,*vi), OMEGA, (*vi).P()[0], (*vi).P()[1], (*vi).P()[2] );
        FieldInterpolator::XBType* coords[3];
        sinterp.Solve(coords);
        for( CMeshO::VertexIterator vi=surf.vert.begin();vi!=surf.vert.end();vi++ ){
            int vIdx = tri::Index(surf,*vi);
            (*vi).P()[0] = (*coords[0])(vIdx);   
            (*vi).P()[1] = (*coords[1])(vIdx);
            (*vi).P()[2] = (*coords[2])(vIdx);
        }
    #endif

    #if TRUE
        surf.vert.EnableCurvature();
        surf.vert.EnableCurvatureDir();
        tri::UpdateCurvatureFitting<CMeshO>::computeCurvature( surf );
        for(CMeshO::VertexIterator vi = surf.vert.begin(); vi != surf.vert.end(); ++vi){
            (*vi).Kh() = ( (*vi).K1() + (*vi).K2() ) / 2;
        }
    #endif
        
    #if FALSE
        // Enable curvature supports, How do I avoid a
        // double computation of topology here?
        surf.vert.EnableCurvature();
        surf.vert.EnableVFAdjacency();
        surf.face.EnableVFAdjacency();
        surf.face.EnableFFAdjacency();
        vcg::tri::UpdateTopology<CMeshO>::VertexFace( surf );
        vcg::tri::UpdateTopology<CMeshO>::FaceFace( surf );
                      
        //--- Compute curvature and its bounds
        tri::UpdateCurvature<CMeshO>::MeanAndGaussian( surf );
    #endif
        
    if( surf.vert.CurvatureEnabled ){
        //--- Enable color coding
        rm &= ~SURF_FCOLOR;
        rm |= SURF_VCOLOR;
    
        //--- DEBUG compute curvature bounds
        float absmax = -FLT_MAX;
        for(CMeshO::VertexIterator vi = surf.vert.begin(); vi != surf.vert.end(); ++vi){
            float cabs = fabs((*vi).Kh());
            absmax = (cabs>absmax) ? cabs : absmax;
        }
        
        //--- Map curvature to two color ranges:
        //    Blue => Yellow: negative values
        //    Yellow => Red:  positive values
        typedef unsigned char CT;
        for(CMeshO::VertexIterator vi = surf.vert.begin(); vi != surf.vert.end(); ++vi){
            if( (*vi).Kh() < 0 )
                (*vi).C().lerp(Color4<CT>::Yellow, Color4<CT>::Blue, fabs((*vi).Kh())/absmax );
            else
                (*vi).C().lerp(Color4<CT>::Yellow, Color4<CT>::Red, (*vi).Kh()/absmax);
        }
    }
}

// HP: a correspondence has already been executed once!
void Balloon::evolve(){
    // Update iteration counter
    numiterscompleted++;

    //--- THIS IS A DEBUG TEST, ATTEMPTS TO DEBUG
    if( false ){
        //--- Test uniform band update
        for(unsigned int i=0; i<vol.band.size(); i++){
            Point3i& voxi = vol.band[i];
            MyVoxel& v = vol.Voxel(voxi);
            v.sfield += .05;
        }
        //--- Estrai isosurface
        vol.isosurface( surf, 0 );
        //--- Clear band for next isosurface, clearing the corresponding computation field
        for(unsigned int i=0; i<vol.band.size(); i++){
            Point3i& voxi = vol.band[i];
            MyVoxel& v = vol.Voxel(voxi);
            v.status = 0;
            v.face = 0;
            v.index = 0;
            v.field = NAN;
        }
        vol.band.clear();
        //--- Update correspondences & band
        vol.band.reserve(5*surf.fn);
        vol.updateSurfaceCorrespondence( surf, gridAccell, 2*vol.getDelta() );
        return;
    }

    //--- Compute updates from amount stored in vertex quality
    Point3f c; // barycentric coefficients
    Point3f voxp;
    std::vector<float> updates_view(vol.band.size(),0);
    std::vector<float> updates_curv(vol.band.size(),0);
    float view_max_absdst = -FLT_MAX;
    float view_max_dst = -FLT_MAX, view_min_dst = +FLT_MAX;
    float curv_maxval = -FLT_MAX;
    for(unsigned int i=0; i<vol.band.size(); i++){
        Point3i& voxi = vol.band[i];
        MyVoxel& v = vol.Voxel(voxi);
        CFaceO& f = *v.face;
        // extract projected points on surface and obtain barycentric coordinates
        // TODO: double work, it was already computed during correspodence, write a new function?
        Point3f proj;
        vol.off2pos(voxi, voxp);
        vcg::SignedFacePointDistance(f, voxp, proj);
        Triangle3<float> triFace( f.P(0), f.P(1), f.P(2) );

        // Paolo, is this really necessary?
        int axis;
        if     (f.Flags() & CFaceO::NORMX )   axis = 0;
        else if(f.Flags() & CFaceO::NORMY )   axis = 1;
        else                                  axis = 2;
        vcg::InterpolationParameters(triFace, axis, proj, c);

        // Interpolate update amounts & keep track of the range
        if( surf.vert.QualityEnabled ){
            updates_view[i] = c[0]*f.V(0)->Q() + c[1]*f.V(1)->Q() + c[2]*f.V(2)->Q();
            view_max_absdst = (fabs(updates_view[i])>view_max_absdst) ? fabs(updates_view[i]) : view_max_absdst;
            view_max_dst = updates_view[i]>view_max_dst ? updates_view[i] : view_max_dst;
            view_min_dst = updates_view[i]<view_min_dst ? updates_view[i] : view_min_dst;
        }
        // Interpolate curvature amount & keep track of the range
        if( surf.vert.CurvatureEnabled ){
            updates_curv[i] = c[0]*f.V(0)->Kh() + c[1]*f.V(1)->Kh() + c[2]*f.V(2)->Kh();
            curv_maxval = (fabs(updates_curv[i])>curv_maxval) ? fabs(updates_curv[i]) : curv_maxval;
        }
    }
    // Only meaningful if it has been computed..
    qDebug("Delta: %f", vol.getDelta());
        
    if( surf.vert.QualityEnabled )
        qDebug("view distance: min %.3f max %.3f", view_min_dst, view_max_dst);
    if( surf.vert.CurvatureEnabled )
        qDebug("max curvature: %f", curv_maxval);

    //--- Apply exponential functions to modulate and regularize the updates
    float sigma2 = vol.getDelta()*vol.getDelta();
    float k1, k3;

    //--- Maximum speed: avoid over-shoothing by limiting update speed
    // E_view + alpha*E_smooth
    float balance_coeff = .5;
    //--- Max evolution speed proportional to grid size
    float max_speed = vol.getDelta()/2;

    // Slowdown weight, smaller if worst case scenario is very converging
    float k2 = 1 - exp( -powf(view_max_absdst,2) / sigma2 );
        
    for(unsigned int i=0; i<vol.band.size(); i++){
        Point3i& voxi = vol.band[i];
        MyVoxel& v = vol.Voxel(voxi);

        //--- If I know current distance avoid over-shooting, as maximum speed is bound to dist from surface
        if( surf.vert.QualityEnabled )
            max_speed = std::min( vol.getDelta()/2, std::abs(updates_view[i]) );

        //--- Distance weights
        if( surf.vert.QualityEnabled ){             
            // Faster if I am located further
            k1 = exp( -powf(fabs(updates_view[i])-view_max_absdst,2) / sigma2 );
        }
        //--- Curvature weight (faster if spiky)
        if( surf.vert.CurvatureEnabled )
            // k3 = updates_curv[i] / curv_maxval;
            k3 = sign(1.0f,updates_curv[i])*exp(-powf(fabs(updates_curv[i])-curv_maxval,2)/curv_maxval);

        //--- Apply the update on the implicit field
        if( surf.vert.QualityEnabled ){
            float prev = v.sfield;
            if( updates_view[i] == view_min_dst && view_min_dst < 0 ){
                qDebug("UPDATE VALUE %.3f (negative for expansion)", vcg::sign( k1*k2*max_speed, updates_view[i] ) );
                qDebug("d_view: %.2f", updates_view[i]);
                qDebug("k1: %.2f", k1);
                qDebug("k2: %.2f", k2);
                qDebug("max_speed: %.2f", max_speed);
                qDebug("Prev   value %.3f", updates_view[i] );
                qDebug("Curvature update: %.3f", k3*balance_coeff*max_speed );
            }
            v.sfield += vcg::sign( k1*k2*max_speed, updates_view[i] );
            // v.sfield += vcg::sign( .15f*k1*k2*vol.getDelta(), updates_view[i]);
            // v.sfield += .25f * k1 * vol.getDelta();
        }
        // if we don't have computed the distance field, we don't really know how to
        // modulate the laplacian accordingly...
// #if TRUE // ENABLE_CURVATURE_IN_EVOLUTION
//        if( surf.vert.CurvatureEnabled && surf.vert.QualityEnabled ){
        //          v.sfield += .1*k3*k2;
        //        }



        // When we are retro-compensating for over-shooting disable smoothing
        if( surf.vert.CurvatureEnabled && updates_view[i] > 0){
            // qDebug() << " " << k3 << " " << max_speed;
            v.sfield += k3*balance_coeff*max_speed; // prev .1
        } else if( surf.vert.CurvatureEnabled && !surf.vert.QualityEnabled ) {
            v.sfield += k3*balance_coeff*max_speed; // prev .1
        }
// #endif
    }

    //--- DEBUG LINES: what's being updated (cannot put above cause it's in a loop)
    if( surf.vert.QualityEnabled )
      qDebug() << "updating implicit function using distance field";
    if( surf.vert.CurvatureEnabled && surf.vert.QualityEnabled )
      qDebug() << "updating implicit function using (modulated) curvature";  
    else if( surf.vert.CurvatureEnabled )
      qDebug() << "updating implicit function using (unmodulated) curvature";  
       
    //--- Estrai isosurface
    vol.isosurface( surf, 0 );

    //--- Clear band for next isosurface, clearing the corresponding computation field
    for(unsigned int i=0; i<vol.band.size(); i++){
        Point3i& voxi = vol.band[i];
        MyVoxel& v = vol.Voxel(voxi);
        v.status = 0;
        v.face = 0;
        v.index = 0;
        v.field = NAN;
    }
    vol.band.clear();
    //--- Update correspondences & band
    vol.band.reserve(5*surf.fn);
    vol.updateSurfaceCorrespondence( surf, gridAccell, 2*vol.getDelta() );
    //--- Disable curvature and quality
    surf.vert.CurvatureEnabled = false;
    surf.vert.QualityEnabled = false;
}

//---------------------------------------------------------------------------------------//
//
//                                   RENDERING
//
//---------------------------------------------------------------------------------------//
void Balloon::render_cloud(){
    // Draw the ray/rays from their origin up to some distance away
    glDisable(GL_LIGHTING);
    glColor3f(.5, .5, .5);
    for(CMeshO::VertexIterator vi=cloud.vert.begin(); vi!=cloud.vert.end(); ++vi){
        Point3f p1 = (*vi).P();
        Point3f n = (*vi).N();
        // n[0] *= .1; n[1] *= .1; n[2] *= .1; // Scale the viewdir
        Point3f p2 = (*vi).P() + n;
        glBegin(GL_LINES);
            glVertex3f(p1[0],p1[1],p1[2]);
            glVertex3f(p2[0],p2[1],p2[2]);
        glEnd();
    }
    glEnable(GL_LIGHTING);
}
void Balloon::render_isosurface(GLArea* gla){
    GLW::DrawMode       dm = GLW::DMFlatWire;
    GLW::ColorMode      cm = GLW::CMPerVert;
    GLW::TextureMode	tm = GLW::TMNone;

    // By default vertColor is defined, so let's check if we need/want to
    // draw the face colors first.
    if( (rm & SURF_FCOLOR) && tri::HasPerFaceColor(surf) ){
        gla->rm.colorMode = vcg::GLW::CMPerFace; // Corrects MESHLAB BUG
        cm = GLW::CMPerFace;
    }
    else if( (rm & SURF_VCOLOR) && tri::HasPerVertexColor(surf) ){
        gla->rm.colorMode = vcg::GLW::CMPerVert; // Corrects MESHLAB BUG
        cm = GLW::CMPerVert;
    }
    GlTrimesh<CMeshO> surf_renderer;
    surf_renderer.m = &surf;
    surf_renderer.Draw(dm, cm, tm);
}
void Balloon::render_surf_to_acc(){
    gridAccell.render();

#if 0
    glDisable( GL_LIGHTING );
    const float ONETHIRD = 1.0f/3.0f;
    Point3f fcenter;
    Point3i off, o;
    glColor3f(1.0, 0.0, 0.0);
    for(unsigned int fi =0; fi<surf.face.size(); fi++){
        if( fi!= 5 )
            continue;
        CFaceO& f = surf.face[fi];
        fcenter = f.P(0) + f.P(1) + f.P(2);
        fcenter = myscale( fcenter, ONETHIRD );
        gridAccell.pos2off( fcenter, off );
        vol.pos2off( fcenter, o );
        gridAccell.off2pos( off, fcenter );
        vcg::drawBox( fcenter, vol.getDelta()*.95, true );


        // SHO BLOCK
        qDebug() << "full volume: " << endl << vol.toString(2, o[2]);
        // DEBUG: examine field values around current coordinate
        QString q;       
        qDebug();
        q.sprintf("%2.2f %2.2f %2.2f \n%2.2f %2.2f %2.2f \n%2.2f %2.2f %2.2f",
                  vol.Val(o[0]-1,o[1]+1,o[2]), vol.Val(o[0],o[1]+1,o[2]), vol.Val(o[0]+1,o[1]+1,o[2]),
                  vol.Val(o[0]-1,o[1],o[2]),   vol.Val(o[0],o[1],o[2]), vol.Val(o[0]+1,o[1]+0,o[2]),
                  vol.Val(o[0]-1,o[1]-1,o[2]), vol.Val(o[0],o[1]-1,o[2]), vol.Val(o[0]+1,o[1]-1,o[2]) );
        qDebug() << q;
        qDebug();
        o = off; // use the gridaccell data
        q.sprintf("%2.2f %2.2f %2.2f \n%2.2f %2.2f %2.2f \n%2.2f %2.2f %2.2f",
                  vol.Val(o[0]-1,o[1]+1,o[2]), vol.Val(o[0],o[1]+1,o[2]), vol.Val(o[0]+1,o[1]+1,o[2]),
                  vol.Val(o[0]-1,o[1],o[2]),   vol.Val(o[0],o[1],o[2]), vol.Val(o[0]+1,o[1]+0,o[2]),
                  vol.Val(o[0]-1,o[1]-1,o[2]), vol.Val(o[0],o[1]-1,o[2]), vol.Val(o[0]+1,o[1]-1,o[2]) );
        qDebug() << q;
    }
#endif

    glEnable( GL_LIGHTING );
}
void Balloon::render_surf_to_vol(){
    if( !vol.isInit() ) return;
    Point3f p, proj;
    glDisable(GL_LIGHTING);
    for(int i=0;i<vol.size(0);i++)
        for(int j=0;j<vol.size(1);j++)
            for(int k=0;k<vol.size(2);k++){
                // if( i!=3 || j!=6 || k!=5 ) continue;

                MyVoxel& v = vol.Voxel(i,j,k);
                vol.off2pos(i,j,k,p);
                // Only ones belonging to active band
                if( v.status ==  2 ){ // && ( tri::Index(surf, v.face)==35||tri::Index(surf, v.face)==34) ){
                    assert( v.face != 0 );
                    vcg::drawBox(p, .05*vol.getDelta());
                    Point3f proj;
                    // float dist = vcg::SignedFacePointDistance(*v.face, p, proj);
                    vcg::SignedFacePointDistance(*v.face, p, proj);
                    //proj = vcg::Barycenter(*v.face);
                    vcg::drawSegment( p, proj );
                }
            }
    glEnable(GL_LIGHTING);
}
void Balloon::render(GLArea* gla){
    if( rm & SHOW_CLOUD )
        render_cloud();
    if( rm & SHOW_VOLUME )
        vol.render();
    if( rm & SHOW_SURF )
        render_isosurface(gla);
    //if( rm & SHOW_ACCEL )
    //    gridAccell.render();
    if( rm & SHOW_3DDDR )
        render_surf_to_acc();
    if( rm & SHOW_SURF_TO_VOL )
        render_surf_to_vol();
}
