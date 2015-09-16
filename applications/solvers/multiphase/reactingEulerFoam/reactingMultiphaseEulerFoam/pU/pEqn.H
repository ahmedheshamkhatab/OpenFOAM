PtrList<surfaceScalarField> alphafs(fluid.phases().size());
PtrList<volScalarField> rAUs(fluid.phases().size());
PtrList<surfaceScalarField> alpharAUfs(fluid.phases().size());

forAll(fluid.phases(), phasei)
{
    phaseModel& phase = fluid.phases()[phasei];
    const volScalarField& alpha = phase;

    alphafs.set(phasei, fvc::interpolate(alpha).ptr());
    alphafs[phasei].rename("pEqn" + alphafs[phasei].name());

    rAUs.set
    (
        phasei,
        new volScalarField
        (
            IOobject::groupName("rAU", phase.name()),
            1.0
           /(
               UEqns[phasei].A()
             + max(phase.residualAlpha() - alpha, scalar(0))
              *phase.rho()/runTime.deltaT()
            )
        )
    );

    alpharAUfs.set
    (
        phasei,
        (
            fvc::interpolate(max(alpha, phase.residualAlpha())*rAUs[phasei])
        ).ptr()
    );
}

// Turbulent diffusion, particle-pressure, lift and wall-lubrication fluxes
PtrList<surfaceScalarField> phiFs(fluid.phases().size());
{
    autoPtr<PtrList<volVectorField> > Fs = fluid.Fs();

    forAll(fluid.phases(), phasei)
    {
        phaseModel& phase = fluid.phases()[phasei];

        phiFs.set
        (
            phasei,
            new surfaceScalarField
            (
                IOobject::groupName("phiF", phase.name()),
                (fvc::interpolate(rAUs[phasei]*Fs()[phasei]) & mesh.Sf())
            )
        );
    }
}

// --- Pressure corrector loop
while (pimple.correct())
{
    // Update continuity errors due to temperature changes
    fluid.correct();

    PtrList<volVectorField> HbyAs(fluid.phases().size());

    forAll(fluid.phases(), phasei)
    {
        phaseModel& phase = fluid.phases()[phasei];
        const volScalarField& alpha = phase;

        // Correct fixed-flux BCs to be consistent with the velocity BCs
        MRF.correctBoundaryFlux(phase.U(), phase.phi());

        HbyAs.set
        (
            phasei,
            new volVectorField
            (
                IOobject::groupName("HbyA", phase.name()),
                phase.U()
            )
        );

        HbyAs[phasei] =
            rAUs[phasei]
           *(
                UEqns[phasei].H()
              + max(phase.residualAlpha() - alpha, scalar(0))
               *phase.rho()*phase.U().oldTime()/runTime.deltaT()
            );
    }

    // Mean density for buoyancy force and p_rgh -> p
    volScalarField rho("rho", fluid.rho());

    surfaceScalarField ghSnGradRho
    (
        "ghSnGradRho",
        ghf*fvc::snGrad(rho)*mesh.magSf()
    );

    PtrList<surfaceScalarField> phigs(fluid.phases().size());
    forAll(fluid.phases(), phasei)
    {
        phaseModel& phase = fluid.phases()[phasei];

        phigs.set
        (
            phasei,
            (
                alpharAUfs[phasei]
               *(
                   ghSnGradRho
                 - (fvc::interpolate(phase.rho() - rho))*(g & mesh.Sf())
                 - fluid.surfaceTension(phase)*mesh.magSf()
                )
            ).ptr()
        );
    }

    PtrList<surfaceScalarField> phiHbyAs(fluid.phases().size());

    surfaceScalarField phiHbyA
    (
        IOobject
        (
            "phiHbyA",
            runTime.timeName(),
            mesh,
            IOobject::NO_READ,
            IOobject::AUTO_WRITE
        ),
        mesh,
        dimensionedScalar("phiHbyA", dimArea*dimVelocity, 0)
    );

    forAll(fluid.phases(), phasei)
    {
        phaseModel& phase = fluid.phases()[phasei];
        const volScalarField& alpha = phase;

        // ddtPhiCorr filter -- only apply in pure(ish) phases
        surfaceScalarField alphafBar
        (
            fvc::interpolate(fvc::average(alphafs[phasei]))
        );
        surfaceScalarField phiCorrCoeff(pos(alphafBar - 0.99));

        forAll(mesh.boundary(), patchi)
        {
            // Set ddtPhiCorr to 0 on non-coupled boundaries
            if
            (
                !mesh.boundary()[patchi].coupled()
             || isA<cyclicAMIFvPatch>(mesh.boundary()[patchi])
            )
            {
                phiCorrCoeff.boundaryField()[patchi] = 0;
            }
        }

        phiHbyAs.set
        (
            phasei,
            new surfaceScalarField
            (
                IOobject::groupName("phiHbyA", phase.name()),
                (fvc::interpolate(HbyAs[phasei]) & mesh.Sf())
              + phiCorrCoeff
               *fvc::interpolate
                (
                    alpha.oldTime()*phase.rho()().oldTime()*rAUs[phasei]
                )
               *(
                   MRF.absolute(phase.phi().oldTime())
                 - (fvc::interpolate(phase.U().oldTime()) & mesh.Sf())
                )/runTime.deltaT()
              - phiFs[phasei]
              - phigs[phasei]
            )
        );

        forAllConstIter
        (
            phaseSystem::KdTable,
            fluid.Kds(),
            KdIter
        )
        {
            const volScalarField& K(*KdIter());

            const phasePair& pair(fluid.phasePairs()[KdIter.key()]);

            const phaseModel* phase1 = &pair.phase1();
            const phaseModel* phase2 = &pair.phase2();

            forAllConstIter(phasePair, pair, iter)
            {
                if (phase1 == &phase)
                {
                    phiHbyAs[phasei] +=
                        fvc::interpolate(rAUs[phasei]*K)
                       *MRF.absolute(phase2->phi());

                    HbyAs[phasei] += rAUs[phasei]*K*phase2->U();
                }

                Swap(phase1, phase2);
            }
        }

        phiHbyA += alphafs[phasei]*phiHbyAs[phasei];
    }

    MRF.makeRelative(phiHbyA);

    // Construct pressure "diffusivity"
    surfaceScalarField rAUf
    (
        IOobject
        (
            "rAUf",
            runTime.timeName(),
            mesh
        ),
        mesh,
        dimensionedScalar("rAUf", dimensionSet(-1, 3, 1, 0, 0), 0)
    );

    forAll(fluid.phases(), phasei)
    {
        rAUf += alphafs[phasei]*alpharAUfs[phasei];
    }
    rAUf = mag(rAUf);


    // Update the fixedFluxPressure BCs to ensure flux consistency
    {
        surfaceScalarField::GeometricBoundaryField phib(phi.boundaryField());
        phib = 0;
        forAll(fluid.phases(), phasei)
        {
            phaseModel& phase = fluid.phases()[phasei];
            phib += alphafs[phasei].boundaryField()*phase.phi().boundaryField();
        }

        setSnGrad<fixedFluxPressureFvPatchScalarField>
        (
            p_rgh.boundaryField(),
            (
                phiHbyA.boundaryField() - phib
            )/(mesh.magSf().boundaryField()*rAUf.boundaryField())
        );
    }

    PtrList<fvScalarMatrix> pEqnComps(fluid.phases().size());
    forAll(fluid.phases(), phasei)
    {
        phaseModel& phase = fluid.phases()[phasei];

        if (phase.compressible())
        {
            const volScalarField& alpha = phase;
            const volScalarField& rho = phase.rho();

            if (pimple.transonic())
            {
                surfaceScalarField phid
                (
                    IOobject::groupName("phid", phase.name()),
                    fvc::interpolate(phase.thermo().psi())*phase.phi()
                );

                pEqnComps.set
                (
                    phasei,
                    (
                        (
                            phase.continuityError() - fluid.dmdt(phase)
                          - fvc::Sp
                            (
                                fvc::ddt(alpha) + fvc::div(phase.alphaPhi()),
                                rho
                            )
                        )/rho
                      + (alpha/rho)*correction
                        (
                            phase.thermo().psi()*fvm::ddt(p_rgh)
                          + fvm::div(phid, p_rgh)
                          - fvm::Sp(fvc::div(phid), p_rgh)
                        )
                    ).ptr()
                );

                deleteDemandDrivenData
                (
                    pEqnComps[phasei].faceFluxCorrectionPtr()
                );
                pEqnComps[phasei].relax();
            }
            else
            {
                pEqnComps.set
                (
                    phasei,
                    (
                        (
                            phase.continuityError() - fluid.dmdt(phase)
                          - fvc::Sp
                            (
                                (fvc::ddt(alpha) + fvc::div(phase.alphaPhi())),
                                rho
                            )
                        )/rho
                      + (alpha*phase.thermo().psi()/rho)
                       *correction(fvm::ddt(p_rgh))
                    ).ptr()
                );
            }
        }
    }

    // Cache p prior to solve for density update
    volScalarField p_rgh_0(p_rgh);

    // Iterate over the pressure equation to correct for non-orthogonality
    while (pimple.correctNonOrthogonal())
    {
        // Construct the transport part of the pressure equation
        fvScalarMatrix pEqnIncomp
        (
            fvc::div(phiHbyA)
          - fvm::laplacian(rAUf, p_rgh)
        );

        {
            fvScalarMatrix pEqn(pEqnIncomp);

            forAll(fluid.phases(), phasei)
            {
                phaseModel& phase = fluid.phases()[phasei];

                if (phase.compressible())
                {
                    pEqn += pEqnComps[phasei];
                }
            }

            solve
            (
                pEqn,
                mesh.solver(p_rgh.select(pimple.finalInnerIter()))
            );
        }

        // Correct fluxes and velocities on last non-orthogonal iteration
        if (pimple.finalNonOrthogonalIter())
        {
            phi = phiHbyA + pEqnIncomp.flux();

            surfaceScalarField mSfGradp("mSfGradp", pEqnIncomp.flux()/rAUf);

            forAll(fluid.phases(), phasei)
            {
                phaseModel& phase = fluid.phases()[phasei];

                phase.phi() = phiHbyAs[phasei] + alpharAUfs[phasei]*mSfGradp;

                // Set the phase dilatation rates
                if (phase.compressible())
                {
                    phase.divU(-pEqnComps[phasei] & p_rgh);
                }
            }

            // Optionally relax pressure for velocity correction
            p_rgh.relax();

            mSfGradp = pEqnIncomp.flux()/rAUf;

            forAll(fluid.phases(), phasei)
            {
                phaseModel& phase = fluid.phases()[phasei];

                phase.U() =
                    HbyAs[phasei]
                  + fvc::reconstruct
                    (
                        alpharAUfs[phasei]*mSfGradp
                      - phiFs[phasei]
                      - phigs[phasei]
                    );
                phase.U().correctBoundaryConditions();
                fvOptions.correct(phase.U());
            }
        }
    }

    // Update and limit the static pressure
    p = max(p_rgh + rho*gh, pMin);

    // Limit p_rgh
    p_rgh = p - rho*gh;

    // Update densities from change in p_rgh
    forAll(fluid.phases(), phasei)
    {
        phaseModel& phase = fluid.phases()[phasei];
        phase.rho()() += phase.thermo().psi()*(p_rgh - p_rgh_0);
    }

    // Correct p_rgh for consistency with p and the updated densities
    rho = fluid.rho();
    p_rgh = p - rho*gh;
    p_rgh.correctBoundaryConditions();
}