#include "IcpOptimizer.h"

using namespace std;
using namespace Eigen;
using namespace nanoflann;


/*
Main constructor. Initializes the point clouds and the sparse ICP parameters.
*/
IcpOptimizer::IcpOptimizer(Matrix<double,Dynamic,3> _firstCloud, Matrix<double,Dynamic,3> _secondCloud, size_t _kNormals, int _nbIterations, int _nbIterationsIn, double _mu, int _nbIterShrink, double _p, IcpMethod _method, bool _verbose) : 
firstCloud(_firstCloud), secondCloud(_secondCloud), kNormals(_kNormals), nbIterations(_nbIterations), nbIterationsIn(_nbIterationsIn), mu(_mu), nbIterShrink(_nbIterShrink), p(_p), method(_method), verbose(_verbose)
{
  //Normal estimation
  cout << "Estimating normals for first cloud" << endl;
  firstNormals = estimateNormals(_firstCloud,kNormals);
  if(method == pointToPlane)
  {
    cout << "Estimating normals for second cloud" << endl;
    secondNormals = estimateNormals(_secondCloud,kNormals);
    cout << "Done with normal estimation" << endl;
  }

  //Initialize the computed transformation
  computedTransfo = RigidTransfo(RotMatrix::Identity(),TransMatrix::Zero(3,1));

  //Initialize the Lagrange multipliers to 0 for step 2.1
  lambda.resize(firstCloud.rows(),3);
  lambda.setZero();

  //Initialize the other parameter
  hasBeenComputed = false;
}

/*
This function is the main implementation of the algorithm where every step are made explicit.
*/
int IcpOptimizer::performSparceICP()
{
  //Initialize the point cloud that is going to move
  movingPC = firstCloud;
  movingNormals = firstNormals;

  //Beginning of the algorithm itself
  for(int iter = 0; iter<nbIterations ; iter++)
  {
    cout << "Iteration " << iter << endl;
    //1st step : Computing correspondances
    PointCloud matchPC = computeCorrespondances(secondCloud,movingPC); //y in the paper

    //2nd step : Computing transformation
    RigidTransfo iterTransfo;
    for(int iterTwo = 0; iterTwo<nbIterationsIn;iterTwo++)
    {
      // step 2.1 Computing z
    
      //Compute h
      PointCloud h = movingPC-matchPC+lambda/mu;
      //Optimizing z with the shrink operator
      PointCloud z = PointCloud::Zero(h.rows(),3);
      for(int i=0;i<h.rows();i++)
        z.row(i) = shrink(h.row(i).transpose());

      // step 2.2 point-to-point ICP

      //Compute C
      PointCloud c = matchPC + z - lambda/mu;
      //Make a standard ICP iteration
      if(method == pointToPoint)
        iterTransfo = rigidTransformPointToPoint(movingPC,c);
      else if(method == pointToPlane)
        iterTransfo = rigidTransformPointToPlane(movingPC,c,secondNormals);
      else
        cout << "Warning ! The method you try to use is incorrect !" << endl;

      //Updating the moving pointCloud
      movingPC = movePointCloud(movingPC,iterTransfo);
      movingNormals = (iterTransfo.first*movingNormals.transpose()).transpose();
      computedTransfo = compose(iterTransfo,computedTransfo);

      // step 2.3 Updating the Lagrange multipliers
      PointCloud delta = movingPC-matchPC - z;
      lambda = lambda + mu * delta;
    }
  }
  
  hasBeenComputed = true;
  return 0;
}

/* 
This function computes each closest point in refCloud for each point in queryCloud using the nanoflann kd-tree implementation. It retunes the point cloud of the closest points of queryCloud.
*/
PointCloud IcpOptimizer::computeCorrespondances(Matrix<double,Dynamic,3> refCloud, Matrix<double,Dynamic,3> queryCloud)
{
  //Create an adapted kd tree for the point cloud
  typedef KDTreeEigenMatrixAdaptor< Matrix<double,Dynamic,3> > my_kd_tree_t;

  //Create an index
  my_kd_tree_t   mat_index(3, refCloud, 10 /* max leaf */ );
  mat_index.index->buildIndex();

  PointCloud nearestPoints = PointCloud::Zero(queryCloud.rows(),3);
  for(int i=0;i<queryCloud.rows();i++)
  {
    //Current query point
    Matrix<double,1,3> queryPoint = queryCloud.block(i,0,1,3);

    //Do a knn search
    const size_t num_results = 1; //We want the nearest neighbour
    vector<size_t>   ret_indexes(num_results);
    vector<double> out_dists_sqr(num_results);

    KNNResultSet<double> resultSet(num_results);
    resultSet.init(&ret_indexes[0], &out_dists_sqr[0] );

    mat_index.index->findNeighbors(resultSet, &queryPoint[0], SearchParams(10));

    //Updating the resulting point cloud
    nearestPoints.row(i) = refCloud.row(ret_indexes[0]);

    if(verbose)
    {
      cout << queryPoint(0,0) << " " << queryPoint(0,1) << " " << queryPoint(0,2) << " refPoint" << endl;
      cout << refCloud(ret_indexes[0],0) << " " << refCloud(ret_indexes[0],1) << " " << refCloud(ret_indexes[0],2) << " closestPoint" << endl << endl << endl;
    }
  }
  return nearestPoints;
}

/*
Move the pointCloud according to the rigid transformation in t 
*/
PointCloud IcpOptimizer::movePointCloud(PointCloud pointCloud, RigidTransfo t)
{
  return (t.first*pointCloud.transpose()+t.second.replicate(1,pointCloud.rows())).transpose();
}

/* 
This function estimates the normals for the point cloud pointCloud. It makes use of the k nearest neighbour algorithm implemented in FLANN
*/
Matrix<double,Dynamic,3> IcpOptimizer::estimateNormals(Matrix<double,Dynamic,3> pointCloud, const size_t k)
{
  //Create an adapted kd tree for the point cloud
  typedef KDTreeEigenMatrixAdaptor< Matrix<double,Dynamic,3> > my_kd_tree_t;

  //Create an index
  my_kd_tree_t   mat_index(3, pointCloud, 10 /* max leaf */ );
  mat_index.index->buildIndex();

  Matrix<double,Dynamic,3> normals;
  normals.resize(pointCloud.rows(),3);
  for(int i=0;i<pointCloud.rows();i++)
  {
    //Current point for which the normal is being computed
    Matrix<double,1,3> currentPoint = pointCloud.block(i,0,1,3);
    //cout << "Current point : " << currentPoint << endl;
    
    //Do a knn search
    vector<size_t> ret_indexes(k);
    vector<double> out_dists_sqr(k);

    KNNResultSet<double> resultSet(k);
    resultSet.init(&ret_indexes[0], &out_dists_sqr[0]);

    mat_index.index->findNeighbors(resultSet, &currentPoint[0], SearchParams(10));
    
    //Compute the covariance matrix

    //Compute the barycentre of the nearest neighbours
    Matrix<double,1,3> barycentre;
    for(int j=0;j<3;j++)
    {
      double curVal = 0.;
      for(int neighbour=0;neighbour<k;neighbour++)
        curVal += pointCloud(ret_indexes[neighbour],j);
      barycentre(0,j) = curVal / double(k);
    }
    
    //Compute the centered nearest neighbour matrix
    Matrix<double,Dynamic,3> centeredNN;
    centeredNN.resize(k,3);
    for(int j=0;j<k;j++)
      centeredNN.row(j) = pointCloud.row(ret_indexes[j])-barycentre;

    //Compute the covariance matrix
    Matrix<double,3,3> covariance = centeredNN.transpose()*centeredNN;

    //Computing its eigen values
    EigenSolver<Matrix<double,Dynamic,Dynamic> > eigenSolver(covariance);

    //Find the indice of the lowest eigen value
    int bestIndice = -1;
    double bestVal = DBL_MAX;
    for(int j=0;j<3;j++)
      if(eigenSolver.eigenvalues()(j,0).real()<bestVal)
      {
        bestVal = eigenSolver.eigenvalues()(j,0).real();
        bestIndice = j;
      }

    //Filling the normal
    normals.row(i) = eigenSolver.eigenvectors().block(0,bestIndice,3,1).normalized().transpose().real();
  }
  return normals;
}

/*
This function is the standard point to plane ICP
a : moving cloud
b : reference cloud
*/
RigidTransfo IcpOptimizer::rigidTransformPointToPoint(PointCloud a, PointCloud b) const
{
  //Centering the point clouds
  Matrix<double,1,3> centerA = a.colwise().sum()/a.rows();
  Matrix<double,1,3> centerB = b.colwise().sum()/b.rows();
  PointCloud aCenter = a - centerA.replicate(a.rows(),1);
  PointCloud bCenter = b - centerB.replicate(b.rows(),1);

  //Computing the cross product matrix W
  Matrix<double,3,3> W;
  W.setZero();
  for(int i=0;i<a.rows();i++)
    W = W + a.row(i).transpose()*b.row(i);

  //Computing singular value decomposition
  JacobiSVD<Matrix<double,3,3>> svd(W, Eigen::ComputeFullU | Eigen::ComputeFullV);

  //Computing the rigid transformation
  RotMatrix rotation = svd.matrixV()*svd.matrixU().transpose();
  TransMatrix translation = (centerB.transpose()-rotation*centerA.transpose()).transpose();

  //Outputing the transformation
  if(verbose)
  { 
    cout << endl << endl << "Rotation Matrix : " << endl << rotation << endl;
    cout << "Translation Matrix : " << endl << translation << endl << endl << endl;
  }

  return RigidTransfo(rotation,translation);
}

RigidTransfo IcpOptimizer::rigidTransformPointToPlane(PointCloud a, PointCloud b, Matrix<double,Dynamic,3> n) const
{

  //TODO

  return RigidTransfo(RotMatrix::Identity(),TransMatrix::Zero(3,1));
}

/*
This function implements the shrink operator which optimizes the function
f(z) = ||z||_2^p + mu/2*||z-h||_2^2
*/
TransMatrix IcpOptimizer::shrink(TransMatrix h) const
{
  double alpha_a = pow((2./mu)*(1.-p),1./(2.-p));
  double hTilde = alpha_a+(p/mu)*pow(alpha_a,p-1);
  double hNorm = h.norm();
  if(hNorm <= hTilde)
    return 0*h;
  double beta = ((alpha_a)/hNorm+1.)/2.;
  for(int i=0;i<nbIterShrink;i++)
    beta = 1 - (p/mu)*pow(hNorm,p-2.)*pow(beta,p-1);
  return beta*h;
}

/*
Computing composition of tNew by tOld (tNew o tOld)
*/
RigidTransfo IcpOptimizer::compose(RigidTransfo tNew, RigidTransfo tOld) const
{
  return RigidTransfo(tNew.first*tOld.first,tNew.first*tOld.second+tNew.second);
}

/* 
Just a getter to the normals of the first cloud (moving cloud)
*/
Matrix<double,Dynamic,3> IcpOptimizer::getFirstNormals() const
{
  return firstNormals;
}

/*
Return a copy of the first point cloud which has been moved by the computed 
rigid motion.
If the rigid motion has not been computed it returns just the original first point cloud.
*/
PointCloud IcpOptimizer::getMovedNormals() const
{
  if(hasBeenComputed)
    return firstNormals;
  else
  {
    cout << "Warning ! The transformation has not been computed ! Please use the method \
    performSparceICP() before retrieving the moved normals." << endl;
    return movingNormals;
  }
}

/*
Return a copy of the first point cloud which has been moved by the computed 
rigid motion.
If the rigid motion has not been computed it returns just the original first point cloud.
*/
PointCloud IcpOptimizer::getMovedPointCloud() const
{
  if(hasBeenComputed)
    return movingPC;
  else
  {
    cout << "Warning ! The transformation has not been computed ! Please use the method \
    performSparceICP() before retrieving the moved point cloud." << endl;
    return firstCloud;
  }
}

/*
Return the computed transformation.
If it has not been computed, just returns the identity.
*/
RigidTransfo IcpOptimizer::getComputedTransfo() const
{
  if(!hasBeenComputed)
    cout << "Warning ! The transformation has not been computed ! Please use the method \
      performSparceICP() before retrieving the rigid motion." << endl;
  return computedTransfo; 
}

