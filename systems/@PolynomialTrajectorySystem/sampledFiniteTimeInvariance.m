function Vtraj = sampledFiniteTimeInvariance(sys,G,Vtraj0,ts,options)

% Attempts to find the largest funnel, defined by the time-varying
% one-level set of Vtraj, which verifies (using SOS over state at finite 
% sample points in time) initial conditions to end inside the one-level set
% of the goal region G at time ts(end).  
% 
% Implements the algorithm described in http://arxiv.org/pdf/1010.3013v1

x=sys.p_x;
t=msspoly('t',1);
ts=ts(:);

num_x = sys.getNumStates();
num_xd = sys.getNumDiscStates();
num_xc = sys.getNumContStates();
if (num_xd), xd = x(1:num_xd); end
x = x(num_xd + (1:num_xc));
num_u = sys.getNumInputs();
u = zeros(num_u,1);

if (nargin<5) options = struct(); end
if (~isfield(options,'rho0_tau')) options.rho0_tau=1; end
if (~isfield(options,'max_iterations')) options.max_iterations=10; end
if (~isfield(options,'converged_tol')) options.converged_tol=.01; end
if (~isfield(options,'stability')) options.stability=false; end  % true implies that we want exponential stability

if (isnumeric(G) && ismatrix(G) && all(size(G)==[num_x,num_x]))
  G = x'*G*x;
end
typecheck(G,'msspoly');
sizecheck(G,[1 1]);

typecheck(Vtraj0,'PolynomialTrajectory');
sizecheck(Vtraj0,[1 1]);


% for now, let's require that G matches V at the final conditions
if (any(abs(gets(G)-gets(subs(Vtraj0.eval(ts(end)),t,ts(end))))>1e-4))
  error('for now, I require that G matches V at the final conditions');
end
rhof = 1;   % todo: handle the more general case and get it from containment

N = length(ts);
Vmin = zeros(N-1,1);

% evaluate dynamics and Vtraj at every ts once (for efficiency/clarity)
for i=1:N
  V{i}=Vtraj0.eval(ts(i));
  f=sys.p_dynamics_traj.eval(ts(i));
  if (num_u)
    f=subs(f,sys.p_u,u);
  end
  if (num_xd>0)
    xn=sys.p_update_traj.eval(ts(i));
    [a,b,c]=decomp(xn - sys.p_x(1:num_xd));
    if ~(isempty(a) && isempty(b) && isempty(c))
      error('discrete states are not supported yet (unless they''re constants)');
    end
    f=subs(f,xd,zeros(num_xd,1));  
  end
  Vdot{i}=diff(V{i},x)*f + Vtraj0.deriv(ts(i));

  % balancing 
  S1=.5*doubleSafe(subs(diff(diff(V{i},x)',x),x,0*x));
  S2=.5*doubleSafe(subs(diff(diff(Vdot{i},x)',x),x,0*x));
  [T,D] = balanceQuadForm(S1,S2);
  
  V{i}=subss(V{i},x,T*x);
  Vdot{i}=subss(Vdot{i},x,T*x);
  
  Vmin(i) = minimumV(x,V{i});
end

if (~isfield(options,'monom_order'))
  options.monom_order = deg(Vdot{1},x);  % just a guess
end

% conservative initial guess (need to do something smarter here)
dts = diff(ts);
rho = flipud(rhof*exp(-options.rho0_tau*(ts-ts(1))/(ts(end)-ts(1))))+max(Vmin);
rhodot = diff(rho)./dts;

% check accuracy by sampling
for i=1:N-1
  m(i)=sampleCheck(x,V{i},Vdot{i},rho(i),rhodot(i));
end
if (max(m)>0)
  figure(1);fnplt(foh(ts,rho')); 
  figure(5);plot(ts(1:end-1),m); drawnow;
  error('infeasible rho. increase options.rho0tau');
end

% perform bilinear search the actual verification here
rhointegral=0;
for iter=1:options.max_iterations
  last_rhointegral = rhointegral; 
  L=findMultipliers(x,V,Vdot,rho,rhodot,options);
  [rho,rhointegral]=optimizeRho(x,V,Vdot,L,dts,Vmin,rhof,options);
  rhodot = diff(rho)./dts;
  rhopp=foh(ts,rho');

  % plot current rho
  figure(10); fnplt(rhopp); title(['iteration ',num2str(iter)]); drawnow;

  % check for convergence
  if ((rhointegral - last_rhointegral) < options.converged_tol*last_rhointegral)  % see if it's converged
    break;
  end
end

% check accuracy by sampling
for i=1:N-1
  m(i)=sampleCheck(x,V{i},Vdot{i},rho(i),rhodot(i));
end
if (max(m)>0)
  figure(5);plot(ts(1:end-1),m); drawnow;
  error('infeasible rho.  increase c');
end

Vtraj = PolynomialTrajectory(@(t) Vtraj0.eval(t)/ppvalSafe(rhopp,t),unique([Vtraj0.getBreaks(),ts']));


end



% fix lagrange multipliers, optimize rho
function [rho,rhointegral]=optimizeRho(x,V,Vdot,L,dts,Vmin,rhof,options)
  N = length(V)-1;
  prog = mssprog;
  [prog,rho] = new(prog,N,'pos');
  rho = [rho;rhof]+Vmin;
  
  rhointegral=0;
  for i=1:N
    rhodot(i,1) = (rho(i+1)-rho(i))/dts(i);
    rhointegral = rho(i)*dts(i)+.5*rhodot(i)*dts(i)^2;

    if (options.stability)
      Vdot{i} = Vdot{i}*rho(i)-V{i}*rhodot(i);
      prog.sos = -Vdot{i} + L{i}*(V{i}-rho(i)); 
    else
      prog.sos = -(Vdot{i}-rhodot(i)+L{i}*(V{i}-rho(i)));
    end
  end
%  prog = sedumi(prog,ones(1,N)*slack-rhointegral,1,struct());
  [prog,info] = sedumi(prog,-rhointegral,0); %1,struct());
  if (info.numerr>1)
    keyboard;
    error('sedumi failed due to numerical errors'); 
  end
  rho = doubleSafe(prog(rho));
  rhointegral = doubleSafe(prog(rhointegral));
end


% fix rho, optimize lagrange multipliers
function L=findMultipliers(x,V,Vdot,rho,rhodot,options)
  % note: compute L for each sample point in parallel using parfor

  N = length(V)-1;
  if (matlabpool('size')==0) matlabpool; end
 
  parfor i=1:N
    prog = mssprog;
    Lxmonom = monomials(x,0:options.monom_order);
    [prog,l] = new(prog,length(Lxmonom),'free');
    L1 = l'*Lxmonom;
    
    [prog,gamma] = new(prog,1,'free');
    if (options.stability)

%      [prog,l2] = new(prog,length(Lxmonom),'free');
%      L2 = l2'*Lxmonom;
      
      Vdot{i} = Vdot{i}*rho(i)-V{i}*rhodot(i);  % intentionally skip 1/rho^2 term
      prog.sos = gamma*V{i} - Vdot{i} + L1*(V{i}-rho(i));% + L2*(.1-V{i});
      prog.sos = L1;
%      prog.sos = L2;
    else
      prog.sos = gamma-(Vdot{i}-rhodot(i) + L1*(V{i}-rho(i)));
    end
    
    [prog,info{i}] = sedumi(prog,gamma,0);
    if (info{i}.pinf==0 && info{i}.dinf==0)
      slack{i}=double(prog(gamma));
    end
    L{i} = prog(L1);
  end
  
  for i=fliplr(1:N)
    if (slack{i}>1e-4 || info{i}.pinf~=0 || info{i}.dinf~=0)
      if (length(x)~=2)
        dims=[2;4]; d=ones(length(x),1); d(dims)=0; d=logical(d);
        [m,b]=minimumV(x,V{i});
        V{i}=subs(V{i},x(d),b(d));
        Vdot{i}=subs(Vdot{i},x(d),b(d));
        x=x(dims);
      end
        figure(2); clf; [minVmRho,junk]=plotPoly(x,V{i}-rho(i));
      if (options.stability)
        figure(1); clf; [junk,maxVdot]=plotPoly(x,Vdot{i});
        fprintf(1,'segment %d of %d, slack=%f,\n sampledmax(d/dt(V/rho))=%f, sampledmin(V-rho)=%f\n info:\n',i,N,maxVdot,minVmRho,slack{i});
      else
        figure(1); clf; [junk,maxVdotmRhodot]=plotPoly(x,Vdot{i}-rhodot(i));
        fprintf(1,'segment %d of %d, slack=%f,\n sampledmax(Vdot-rhodot)=%f, sampledmin(V-rho)=%f\n info:\n',i,N,maxVdotmRhodot,minVmRho,slack{i});
      end
      info{i}
      error('rho is infeasible');
    end
  end
end


function [mi,ma]=plotPoly(x,P,rho)
  if(nargin<3) rho=0; end
  [X1,X2]=ndgrid(-2:.1:2,-2:.1:2);
  Ps=reshape(doubleSafe(msubs(P,x,[X1(:)';X2(:)'])),size(X1));
  mi=min(min(Ps));
  ma=max(max(Ps));
  surf(X1,X2,Ps); colorbar;
  view(0,90);
  hold on;
  [c,h]=contour3(X1,X2,Ps,[rho,rho]);
  set(h,'EdgeColor',[1 1 1],'LineWidth',4);
end

function [Vmin,b] = minimumV(x,V)
  if (deg(V,x)>2) 
    prog = mssprog;
    [prog,slack] = new(prog,1,'free');
    prog.sos = slack + V;
    [prog,info] = sedumi(prog,slack,0);
    Vmin = -doubleSafe(prog(slack));
  else
    H = doubleSafe(0.5*diff(diff(V,x)',x));
    b = -0.5*(H\doubleSafe(subs(diff(V,x),x,0*x)'));
    Vmin = subs(V,x,b);
  end    
end

function m=sampleCheck(x,V,Vdot,rho,rhodot)
  if (deg(V,x)>2) error('only checks quadratics'); end
  
  n=length(x);
  K=100;
  X = randn(n,K);
  X = X./repmat(sqrt(sum(X.^2,1)),n,1);

  H = doubleSafe(0.5*diff(diff(V,x)',x));
  b = -0.5*(H\doubleSafe(subs(diff(V,x),x,0*x)'));

  try 
  X = repmat(b,1,K) + (H/(doubleSafe(rho-subs(V,x,b))+eps))^(-1/2)*X;
  catch
    keyboard;
  end
  m=max(doubleSafe(msubs(Vdot,x,X))) - rhodot;
  if (m>0)
    warning('found a positive Vdot');
  end
end


function y=doubleSafe(x)
  y=double(x);
  if (~isa(y,'double')) error('double failed'); end
end

