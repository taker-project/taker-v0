from setuptools import setup, find_packages

with open('README.md', 'r') as file:
    long_description = file.read()

setup(
    name='taker',
    version='0.0.0a0',
    author='Alexander Kernozhitsky',
    author_email='sh200105@mail.ru',
    description='Taker is a system to prepare competitive programming '
                'problems',
    long_description=long_description,
    long_description_content_type='text/markdown',
    url='https://github.com/taker-project/taker_v0',
    classifiers=[
        'Development Status :: 2 - Pre-Alpha',
        'Environment :: Console',
        'License :: OSI Approved :: '
        'GNU General Public License v3 or later (GPLv3+)',
        'Operating System :: POSIX',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.5',
        'Programming Language :: Python :: 3.6',
        'Programming Language :: Python :: 3.7'
    ],
    package_dir={'': 'src'},
    packages=find_packages('src', exclude=['pytest_fixtures', '*.tests',
                                           '*.tests.*']),
    install_requires=['colorama', 'appdirs'],
    entry_points={
        'console_scripts': [
            'take = taker:main'
        ]
    }
)
