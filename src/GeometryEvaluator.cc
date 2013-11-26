#include "GeometryEvaluator.h"
#include "traverser.h"
#include "tree.h"
#include "GeometryCache.h"
#include "CGALCache.h"
#include "Polygon2d.h"
#include "module.h"
#include "state.h"
#include "transformnode.h"
#include "linearextrudenode.h"
#include "rotateextrudenode.h"
#include "csgnode.h"
#include "cgaladvnode.h"
#include "projectionnode.h"
#include "CGAL_Nef_polyhedron.h"
#include "cgalutils.h"
#include "rendernode.h"
#include "clipper-utils.h"
#include "polyset-utils.h"
#include "CGALEvaluator.h"
#include "CGALCache.h"
#include "PolySet.h"
#include "openscad.h" // get_fragments_from_r()
#include "printutils.h"
#include "svg.h"
#include "dxfdata.h"

#include <algorithm>
#include <boost/foreach.hpp>

GeometryEvaluator::GeometryEvaluator(const class Tree &tree):
	tree(tree)
{
	this->cgalevaluator = new CGALEvaluator(tree, *this);
}

/*!
  Factory method returning a Geometry from the given node. If the
  node is already cached, the cached Geometry will be returned
  otherwise a new Geometry will be created from the node. If cache is
  true, the newly created Geometry will be cached.

	FIXME: This looks redundant
 */
shared_ptr<const Geometry> GeometryEvaluator::getGeometry(const AbstractNode &node, bool cache)
{
	std::string cacheid = this->tree.getIdString(node);

	if (GeometryCache::instance()->contains(cacheid)) {
#ifdef DEBUG
    // For cache debugging
		PRINTB("GeometryCache hit: %s", cacheid.substr(0, 40));
#endif
		return GeometryCache::instance()->get(cacheid);
	}

	shared_ptr<const Geometry> geom(this->evaluateGeometry(node));

	if (cache) GeometryCache::instance()->insert(cacheid, geom);
	return geom;
}

bool GeometryEvaluator::isCached(const AbstractNode &node) const
{
	return GeometryCache::instance()->contains(this->tree.getIdString(node));
}

// FIXME: This doesn't insert into cache. Fix this here or in client code
shared_ptr<const Geometry> GeometryEvaluator::evaluateGeometry(const AbstractNode &node)
{
	if (!isCached(node)) {
		Traverser trav(*this, node, Traverser::PRE_AND_POSTFIX);
		trav.execute();
		return this->root;
	}
	return GeometryCache::instance()->get(this->tree.getIdString(node));
}

Geometry *GeometryEvaluator::applyToChildren(const AbstractNode &node, OpenSCADOperator op)
{
	unsigned int dim = 0;
	BOOST_FOREACH(const ChildItem &item, this->visitedchildren[node.index()]) {
		if (item.second) {
			if (!dim) dim = item.second->getDimension();
			else if (dim != item.second->getDimension()) {
				return NULL;
			}
		}
	}
	if (dim == 2) return applyToChildren2D(node, op);
	else if (dim == 3) return applyToChildren3D(node, op);
	return NULL;
}

Geometry *GeometryEvaluator::applyToChildren3D(const AbstractNode &node, OpenSCADOperator op)
{
	CGAL_Nef_polyhedron *N = new CGAL_Nef_polyhedron;
	BOOST_FOREACH(const ChildItem &item, this->visitedchildren[node.index()]) {
		const AbstractNode *chnode = item.first;
		const shared_ptr<const Geometry> &chgeom = item.second;
		// FIXME: Don't use deep access to modinst members
		if (chnode->modinst->isBackground()) continue;
		
		shared_ptr<const CGAL_Nef_polyhedron> chN = dynamic_pointer_cast<const CGAL_Nef_polyhedron>(chgeom);
		if (!chN) {
			shared_ptr<const PolySet> chP = dynamic_pointer_cast<const PolySet>(chgeom);
			if (chP) chN.reset(createNefPolyhedronFromGeometry(*chP));
		}

    // NB! We insert into the cache here to ensure that all children of
    // a node is a valid object. If we inserted as we created them, the 
    // cache could have been modified before we reach this point due to a large
    // sibling object. 
		if (!CGALCache::instance()->contains(this->tree.getIdString(*chnode))) {
            CGALCache::instance()->insert(this->tree.getIdString(*chnode), chN ? *chN : CGAL_Nef_polyhedron());
		}

		if (chgeom) {
			if (chgeom->getDimension() == 3) {
				// Initialize N on first iteration with first expected geometric object
				if (N->isNull() && !N->isEmpty()) *N = chN->copy();
				else CGAL_binary_operator(*N, *chN, op);
			}
			else {
				// FIXME: Fix error message
				PRINT("ERROR: this operation is not defined for 2D child objects!");
			}
		}
		chnode->progress_report();
	}
	return N;
}


/*!
	
*/
Geometry *GeometryEvaluator::applyToChildren2D(const AbstractNode &node, OpenSCADOperator op)
{
	ClipperLib::Clipper sumclipper;
	bool first = true;
	BOOST_FOREACH(const ChildItem &item, this->visitedchildren[node.index()]) {
		const AbstractNode *chnode = item.first;
		const shared_ptr<const Geometry> &chgeom = item.second;
		// FIXME: Don't use deep access to modinst members
		if (chnode->modinst->isBackground()) continue;
		
		// NB! We insert into the cache here to ensure that all children of
		// a node is a valid object. If we inserted as we created them, the 
		// cache could have been modified before we reach this point due to a large
		// sibling object. 
		if (!isCached(*chnode)) {
			GeometryCache::instance()->insert(this->tree.getIdString(*chnode), chgeom);
		}
		
		if (chgeom) {
			if (chgeom->getDimension() == 2) {
				shared_ptr<const Polygon2d> polygons = dynamic_pointer_cast<const Polygon2d>(chgeom);
				assert(polygons);
				// The first Clipper operation will sanitize the polygon, ensuring 
				// contours/holes have the correct winding order
				ClipperLib::Polygons result = ClipperUtils::fromPolygon2d(*polygons);
				result = ClipperUtils::process(result, 
																			 ClipperLib::ctUnion, 
																			 ClipperLib::pftEvenOdd);

				// Add correctly winded polygons to the main clipper
				sumclipper.AddPolygons(result, first ? ClipperLib::ptSubject : ClipperLib::ptClip);
			}
			else {
				// FIXME: Wrong error message
				PRINT("ERROR: linear_extrude() is not defined for 3D child objects!");
			}
		}
		chnode->progress_report();
		if (first) first = !first;
	}

	ClipperLib::ClipType clipType;
	switch (op) {
	case OPENSCAD_UNION:
		clipType = ClipperLib::ctUnion;
		break;
	case OPENSCAD_INTERSECTION:
		clipType = ClipperLib::ctIntersection;
		break;
	case OPENSCAD_DIFFERENCE:
		clipType = ClipperLib::ctDifference;
		break;
	default:
		PRINTB("Error: Unknown boolean operation %d", int(op));
		return NULL;
		break;
	}
	// Perform the main op
	ClipperLib::Polygons sumresult;
	sumclipper.Execute(clipType, sumresult, ClipperLib::pftEvenOdd, ClipperLib::pftEvenOdd);

	if (sumresult.size() == 0) return NULL;

	// The returned result will have outlines ordered according to whether 
	// they're positive or negative: Positive outlines counter-clockwise and 
	// negative outlines clockwise.
	// FIXME: We might want to introduce a flag in Polygon2d to signify this
	return ClipperUtils::toPolygon2d(sumresult);
}

/*!
	Adds ourself to out parent's list of traversed children.
	Call this for _every_ node which affects output during traversal.
	Usually, this should be called from the postfix stage, but for some nodes, 
	we defer traversal letting other components (e.g. CGAL) render the subgraph, 
	and we'll then call this from prefix and prune further traversal.

	The added geometry can be NULL if it wasn't possible to evaluate it.
*/
void GeometryEvaluator::addToParent(const State &state, 
																		const AbstractNode &node, 
																		const shared_ptr<const Geometry> &geom)
{
	this->visitedchildren.erase(node.index());
	if (state.parent()) {
		this->visitedchildren[state.parent()->index()].push_back(std::make_pair(&node, geom));
	}
	else {
		// Root node, insert into cache
		if (!isCached(node)) {
			if (!GeometryCache::instance()->insert(this->tree.getIdString(node), geom)) {
				PRINT("WARNING: GeometryEvaluator: Root node didn't fit into cache");
			}
		}
		this->root = geom;
	}
}

/*!
   Custom nodes are handled here => implicit union
*/
Response GeometryEvaluator::visit(State &state, const AbstractNode &node)
{
	if (state.isPrefix() && isCached(node)) return PruneTraversal;
	if (state.isPostfix()) {
		shared_ptr<const class Geometry> geom;
		if (!isCached(node)) {
			const Geometry *geometry = applyToChildren(node, OPENSCAD_UNION);
			geom.reset(geometry);
		}
		else {
			geom = GeometryCache::instance()->get(this->tree.getIdString(node));
		}
		addToParent(state, node, geom);
	}
	return ContinueTraversal;
}

/*
 FIXME: Where do we handle nodes which should be sent to CGAL?
 
 if (state.isPrefix() && isCached(node)) return PruneTraversal;
 if (state.isPostfix()) {
 shared_ptr<const Geometry> geom;
 if (!isCached(node)) {
 CGAL_Nef_polyhedron N = this->cgalevaluator->evaluateCGALMesh(node);
 CGALCache::instance()->insert(this->tree.getIdString(node), N);
 
 PolySet *ps = NULL;
 if (!N.isNull()) ps = N.convertToPolyset();
 geom.reset(ps);
 }
 else {
 geom = GeometryCache::instance()->get(this->tree.getIdString(node));
 }
 addToParent(state, node, geom);
 }
 return ContinueTraversal;
*/


/*!
	Leaf nodes can create their own geometry, so let them do that

	input: None
	output: PolySet or Polygon2d
*/
Response GeometryEvaluator::visit(State &state, const LeafNode &node)
{
	// FIXME: We should run the result of 2D geometry to Clipper to ensure
	// correct winding order
	if (state.isPrefix()) {
		shared_ptr<const Geometry> geom;
		if (!isCached(node)) {
			const Geometry *geometry = node.createGeometry();
			const Polygon2d *polygons = dynamic_cast<const Polygon2d*>(geometry);
			if (polygons) {
				ClipperLib::Polygons result = ClipperUtils::fromPolygon2d(*polygons);
				result = ClipperUtils::process(result, 
																			 ClipperLib::ctUnion, 
																			 ClipperLib::pftEvenOdd);
				Polygon2d *p = ClipperUtils::toPolygon2d(result);
				delete geometry;
				geometry = p;
			}
			geom.reset(geometry);
		}
		else geom = GeometryCache::instance()->get(this->tree.getIdString(node));
		addToParent(state, node, geom);
	}
	return PruneTraversal;
}

/*!
	input: List of 2D or 3D objects (not mixed)
	output: Polygon2d or 3D PolySet
	operation:
	  o Perform csg op on children
 */			
Response GeometryEvaluator::visit(State &state, const CsgNode &node)
{
	if (state.isPrefix() && isCached(node)) return PruneTraversal;
	if (state.isPostfix()) {
		shared_ptr<const class Geometry> geom;
		if (!isCached(node)) {
			const Geometry *geometry = applyToChildren(node, node.type);
			geom.reset(geometry);
		}
		else {
			geom = GeometryCache::instance()->get(this->tree.getIdString(node));
		}
		addToParent(state, node, geom);
	}
	return ContinueTraversal;
}

/*!
	input: List of 2D or 3D objects (not mixed)
	output: Polygon2d or 3D PolySet
	operation:
	  o Union all children
	  o Perform transform
 */			
Response GeometryEvaluator::visit(State &state, const TransformNode &node)
{
	if (state.isPrefix() && isCached(node)) return PruneTraversal;
	if (state.isPostfix()) {
		shared_ptr<const class Geometry> geom;
		if (!isCached(node)) {
			if (matrix_contains_infinity(node.matrix) || matrix_contains_nan(node.matrix)) {
				// due to the way parse/eval works we can't currently distinguish between NaN and Inf
				PRINT("Warning: Transformation matrix contains Not-a-Number and/or Infinity - removing object.");
			}
			else {
				// First union all children
				Geometry *geometry = applyToChildren(node, OPENSCAD_UNION);
				if (geometry) {
					if (geometry->getDimension() == 2) {
						Polygon2d *polygons = dynamic_cast<Polygon2d*>(geometry);
						assert(polygons);
						Transform2d mat2;
						mat2.matrix() << 
							node.matrix(0,0), node.matrix(0,1), node.matrix(0,3),
							node.matrix(1,0), node.matrix(1,1), node.matrix(1,3),
							node.matrix(3,0), node.matrix(3,1), node.matrix(3,3);
						polygons->transform(mat2);
						geom.reset(polygons);
					}
					else if (geometry->getDimension() == 3) {
						CGAL_Nef_polyhedron *N = dynamic_cast<CGAL_Nef_polyhedron*>(geometry);
						assert(N);
						N->transform(node.matrix);
						geom.reset(N);
					}
				}
			}
		}
		else {
			geom = GeometryCache::instance()->get(this->tree.getIdString(node));
		}
		addToParent(state, node, geom);
	}
	return ContinueTraversal;
}

static void translate_PolySet(PolySet &ps, const Vector3d &translation)
{
	BOOST_FOREACH(PolySet::Polygon &p, ps.polygons) {
		BOOST_FOREACH(Vector3d &v, p) {
			v += translation;
		}
	}
}

static void add_slice(PolySet *ps, const Polygon2d &poly, 
											double rot1, double rot2, 
											double h1, double h2, 
											const Vector2d &scale1,
											const Vector2d &scale2)
{
	Eigen::Affine2d trans1(Eigen::Scaling(scale1) * Eigen::Rotation2D<double>(-rot1*M_PI/180));
	Eigen::Affine2d trans2(Eigen::Scaling(scale2) * Eigen::Rotation2D<double>(-rot2*M_PI/180));
	
	// FIXME: If scale2 == 0 we need to handle tessellation separately
	bool splitfirst = sin(rot1 - rot2) >= 0.0;
	BOOST_FOREACH(const Outline2d &o, poly.outlines()) {
		Vector2d prev1 = trans1 * o[0];
		Vector2d prev2 = trans2 * o[0];
		for (size_t i=1;i<=o.size();i++) {
			Vector2d curr1 = trans1 * o[i % o.size()];
			Vector2d curr2 = trans2 * o[i % o.size()];
			ps->append_poly();
			
			if (splitfirst) {
				ps->insert_vertex(prev1[0], prev1[1], h1);
				ps->insert_vertex(curr2[0], curr2[1], h2);
				ps->insert_vertex(curr1[0], curr1[1], h1);
				if (scale2[0] > 0 || scale2[1] > 0) {
					ps->append_poly();
					ps->insert_vertex(curr2[0], curr2[1], h2);
					ps->insert_vertex(prev1[0], prev1[1], h1);
					ps->insert_vertex(prev2[0], prev2[1], h2);
				}
			}
			else {
				ps->insert_vertex(prev1[0], prev1[1], h1);
				ps->insert_vertex(prev2[0], prev2[1], h2);
				ps->insert_vertex(curr1[0], curr1[1], h1);
				if (scale2[0] > 0 || scale2[1] > 0) {
					ps->append_poly();
					ps->insert_vertex(prev2[0], prev2[1], h2);
					ps->insert_vertex(curr2[0], curr2[1], h2);
					ps->insert_vertex(curr1[0], curr1[1], h1);
				}
			}
			prev1 = curr1;
			prev2 = curr2;
		}
	}
}

/*!
	Input to extrude should be clean. This means non-intersecting, correct winding order
	etc., the input coming from a library like Clipper.
*/
static Geometry *extrudePolygon(const LinearExtrudeNode &node, const Polygon2d &poly)
{
	PolySet *ps = new PolySet();
	ps->setConvexity(node.convexity);
	if (node.height <= 0) return ps;

	double h1, h2;

	if (node.center) {
		h1 = -node.height/2.0;
		h2 = +node.height/2.0;
	} else {
		h1 = 0;
		h2 = node.height;
	}

	PolySet *ps_bottom = poly.tessellate(); // bottom
	
	// Flip vertex ordering for bottom polygon
	BOOST_FOREACH(PolySet::Polygon &p, ps_bottom->polygons) {
		std::reverse(p.begin(), p.end());
	}
	translate_PolySet(*ps_bottom, Vector3d(0,0,h1));

	ps->append(*ps_bottom);
	delete ps_bottom;
	if (node.scale_x > 0 || node.scale_y > 0) {
		Polygon2d top_poly(poly);
		Eigen::Affine2d trans(Eigen::Scaling(node.scale_x, node.scale_y) *
													 Eigen::Rotation2D<double>(-node.twist*M_PI/180));
		top_poly.transform(trans); // top
		PolySet *ps_top = top_poly.tessellate();
		translate_PolySet(*ps_top, Vector3d(0,0,h2));
		ps->append(*ps_top);
		delete ps_top;
	}
    size_t slices = node.has_twist ? node.slices : 1;

	for (int j = 0; j < slices; j++) {
		double rot1 = node.twist*j / slices;
		double rot2 = node.twist*(j+1) / slices;
		double height1 = h1 + (h2-h1)*j / slices;
		double height2 = h1 + (h2-h1)*(j+1) / slices;
		Vector2d scale1(1 - (1-node.scale_x)*j / slices,
										1 - (1-node.scale_y)*j / slices);
		Vector2d scale2(1 - (1-node.scale_x)*(j+1) / slices,
										1 - (1-node.scale_y)*(j+1) / slices);
		add_slice(ps, poly, rot1, rot2, height1, height2, scale1, scale2);
	}

	return ps;
}

/*!
	input: List of 2D objects
	output: 3D PolySet
	operation:
	  o Union all children
	  o Perform extrude
 */			
Response GeometryEvaluator::visit(State &state, const LinearExtrudeNode &node)
{
	if (state.isPrefix() && isCached(node)) return PruneTraversal;
	if (state.isPostfix()) {
		shared_ptr<const Geometry> geom;
		if (!isCached(node)) {
			const Geometry *geometry;
			if (!node.filename.empty()) {
				DxfData dxf(node.fn, node.fs, node.fa, node.filename, node.layername, node.origin_x, node.origin_y, node.scale_x);
				geometry = dxf.toPolygon2d();
			}
			else {
				geometry = applyToChildren2D(node, OPENSCAD_UNION);
			}
			if (geometry) {
				const Polygon2d *polygons = dynamic_cast<const Polygon2d*>(geometry);
				Geometry *extruded = extrudePolygon(node, *polygons);
				assert(extruded);
				geom.reset(extruded);
				delete geometry;
			}
		}
		else {
			geom = GeometryCache::instance()->get(this->tree.getIdString(node));
		}
		addToParent(state, node, geom);
	}
	return ContinueTraversal;
}

static void fill_ring(std::vector<Vector3d> &ring, const Outline2d &o, double a)
{
	for (int i=0;i<o.size();i++) {
		ring[i][0] = o[i][0] * sin(a);
		ring[i][1] = o[i][0] * cos(a);
		ring[i][2] = o[i][1];
	}
}

/*!
	Input to extrude should be clean. This means non-intersecting, correct winding order
	etc., the input coming from a library like Clipper.
*/
static Geometry *rotatePolygon(const RotateExtrudeNode &node, const Polygon2d &poly)
{
	PolySet *ps = new PolySet();
	ps->setConvexity(node.convexity);

	BOOST_FOREACH(const Outline2d &o, poly.outlines()) {
		double max_x = 0;
		BOOST_FOREACH(const Vector2d &v, o) max_x = fmax(max_x, v[0]);
		int fragments = get_fragments_from_r(max_x, node.fn, node.fs, node.fa);

		std::vector<Vector3d> rings[2];
		rings[0].reserve(o.size());
		rings[1].reserve(o.size());

		fill_ring(rings[0], o, -M_PI/2); // first ring
		for (int j = 0; j <= fragments; j++) {
			double a = ((j+1)*2*M_PI) / fragments - M_PI/2; // start on the X axis
			fill_ring(rings[(j+1)%2], o, a);

			for (size_t i=0;i<o.size();i++) {
				ps->append_poly();
				ps->insert_vertex(rings[j%2][i]);
				ps->insert_vertex(rings[(j+1)%2][(i+1)%o.size()]);
				ps->insert_vertex(rings[j%2][(i+1)%o.size()]);
				ps->append_poly();
				ps->insert_vertex(rings[j%2][i]);
				ps->insert_vertex(rings[(j+1)%2][i]);
				ps->insert_vertex(rings[(j+1)%2][(i+1)%o.size()]);
			}
		}
	}
	return ps;
}

/*!
	input: List of 2D objects
	output: 3D PolySet
	operation:
	  o Union all children
	  o Perform extrude
 */			
Response GeometryEvaluator::visit(State &state, const RotateExtrudeNode &node)
{
	if (state.isPrefix() && isCached(node)) return PruneTraversal;
	if (state.isPostfix()) {
		shared_ptr<const Geometry> geom;
		if (!isCached(node)) {
			const Geometry *geometry;
			if (!node.filename.empty()) {
				DxfData dxf(node.fn, node.fs, node.fa, node.filename, node.layername, node.origin_x, node.origin_y, node.scale);
				geometry = dxf.toPolygon2d();
			}
			else {
				geometry = applyToChildren2D(node, OPENSCAD_UNION);
			}
			if (geometry) {
				const Polygon2d *polygons = dynamic_cast<const Polygon2d*>(geometry);
				Geometry *rotated = rotatePolygon(node, *polygons);
				assert(rotated);
				geom.reset(rotated);
				delete geometry;
			}
		}
		else {
			geom = GeometryCache::instance()->get(this->tree.getIdString(node));
		}
		addToParent(state, node, geom);
	}
	return ContinueTraversal;
}

/*!
	Handles non-leaf PolyNodes; projection
*/
Response GeometryEvaluator::visit(State &state, const AbstractPolyNode &node)
{
	assert(false);
}

/*!
	input: List of 3D objects
	output: Polygon2d
	operation:
	  o Union all children
		o Perform projection
 */			
Response GeometryEvaluator::visit(State &state, const ProjectionNode &node)
{
	if (state.isPrefix() && isCached(node)) return PruneTraversal;
	if (state.isPostfix()) {
		shared_ptr<const class Geometry> geom;
		if (!isCached(node)) {

			if (!node.cut_mode) {
				ClipperLib::Clipper sumclipper;
				BOOST_FOREACH(const ChildItem &item, this->visitedchildren[node.index()]) {
					const AbstractNode *chnode = item.first;
					const shared_ptr<const Geometry> &chgeom = item.second;
					// FIXME: Don't use deep access to modinst members
					if (chnode->modinst->isBackground()) continue;

					const Polygon2d *poly = NULL;

// CGAL version of Geometry projection
// Causes crashes in createNefPolyhedronFromGeometry() for this model:
// projection(cut=false) {
//    cube(10);
//    difference() {
//      sphere(10);
//      cylinder(h=30, r=5, center=true);
//    }
// }
#if 0
					shared_ptr<const PolySet> chPS = dynamic_pointer_cast<const PolySet>(chgeom);
					const PolySet *ps2d = NULL;
					shared_ptr<const CGAL_Nef_polyhedron> chN = dynamic_pointer_cast<const CGAL_Nef_polyhedron>(chgeom);
					if (chN) chPS.reset(chN->convertToPolyset());
					if (chPS) ps2d = PolysetUtils::flatten(*chPS);
					if (ps2d) {
						CGAL_Nef_polyhedron *N2d = createNefPolyhedronFromGeometry(*ps2d);
						poly = N2d->convertToPolygon2d();
					}
#endif

// Clipper version of Geometry projection
// Clipper doesn't handle meshes very well.
// It's better in V6 but not quite there. FIXME: stand-alone example.
#if 1
					// project chgeom -> polygon2d
					shared_ptr<const PolySet> chPS = dynamic_pointer_cast<const PolySet>(chgeom);
					if (!chPS) {
						shared_ptr<const CGAL_Nef_polyhedron> chN = dynamic_pointer_cast<const CGAL_Nef_polyhedron>(chgeom);
						if (chN) {
							chPS.reset(chN->convertToPolyset());
						}
					}
					if (chPS) poly = PolysetUtils::project(*chPS);
#endif

					if (poly) {
						ClipperLib::Polygons result = ClipperUtils::fromPolygon2d(*poly);
						// Using NonZero ensures that we don't create holes from polygons sharing
						// edges since we're unioning a mesh
						result = ClipperUtils::process(result, 
																					 ClipperLib::ctUnion, 
																					 ClipperLib::pftNonZero);
						// Add correctly winded polygons to the main clipper
						sumclipper.AddPolygons(result, ClipperLib::ptSubject);
					}
				}
				ClipperLib::Polygons sumresult;
				// This is key - without StrictlySimple, we tend to get self-intersecting results
				sumclipper.StrictlySimple(true);
				sumclipper.Execute(ClipperLib::ctUnion, sumresult, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
				geom.reset(ClipperUtils::toPolygon2d(sumresult));
			}
			else {
				const Geometry *geometry = applyToChildren3D(node, OPENSCAD_UNION);
				if (geometry) {
					const CGAL_Nef_polyhedron *Nptr = dynamic_cast<const CGAL_Nef_polyhedron*>(geometry);
					if (!Nptr) {
						// FIXME: delete this object
						Nptr = createNefPolyhedronFromGeometry(*geometry);
					}
					if (!Nptr->isNull()) {
						CGAL_Nef_polyhedron nef_poly = CGAL_project(*Nptr, node.cut_mode);
						Polygon2d *poly = nef_poly.convertToPolygon2d();
						assert(poly);
						poly->setConvexity(node.convexity);
						geom.reset(poly);
						delete geometry;
					}
				}
			}
		}
		else {
			geom = GeometryCache::instance()->get(this->tree.getIdString(node));
		}
		addToParent(state, node, geom);
	}
	return ContinueTraversal;
}		

/*!
	input: List of 2D or 3D objects (not mixed)
	output: PolySet (FIXME: implement Polygon2d)
	operation:
	  o Perform cgal operation
 */			
Response GeometryEvaluator::visit(State &state, const CgaladvNode &node)
{
	if (state.isPrefix() && isCached(node)) return PruneTraversal;
	if (state.isPostfix()) {
		shared_ptr<const Geometry> geom;
		if (!isCached(node)) {
			// if (node.type == RESIZE) {
			// 	const Geometry *geometry = applyToChildren2D(node, OPENSCAD_UNION);
			// 	// FIXME: find size and transform
			// }
			// else {
				CGAL_Nef_polyhedron N = this->cgalevaluator->evaluateCGALMesh(node);
				PolySet *ps = N.isNull() ? NULL : N.convertToPolyset();
				geom.reset(ps);
//			}
			// FIXME: handle 3D
		}
		else {
			geom = GeometryCache::instance()->get(this->tree.getIdString(node));
		}
		addToParent(state, node, geom);
	}
	return ContinueTraversal;
}

/*!
	input: List of 2D or 3D objects (not mixed)
	output: PolySet (FIXME: implement Polygon2d?)
	operation:
	  o Render to PolySet (using CGAL or Clipper)
 */			
Response GeometryEvaluator::visit(State &state, const RenderNode &node)
{
	if (state.isPrefix() && isCached(node)) return PruneTraversal;
	if (state.isPostfix()) {
		shared_ptr<const Geometry> geom;
		if (!isCached(node)) {
			const Geometry *geometry = applyToChildren(node, OPENSCAD_UNION);
			const CGAL_Nef_polyhedron *N = dynamic_cast<const CGAL_Nef_polyhedron*>(geometry);
			if (N) {
				PolySet *ps = NULL;
				if (!N->isNull()) {
					if (N->getDimension() == 3 && !N->p3->is_simple()) {
						PRINT("WARNING: Body of render() isn't valid 2-manifold!");
					}
					else {
						ps = N->convertToPolyset();
						if (ps) ps->setConvexity(node.convexity);
					}
				}
				geom.reset(ps);
			}
			else {
				geom.reset(geometry);
			}
		}
		else {
			geom = GeometryCache::instance()->get(this->tree.getIdString(node));
		}
		addToParent(state, node, geom);
	}
	return ContinueTraversal;
}